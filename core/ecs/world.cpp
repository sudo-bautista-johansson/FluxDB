#include "../headers/ecs.h"
#include "../headers/query_plans.h"
#include "../headers/delta_set.h"
#include "../headers/rollback.h"
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <cstdio>
#include <iostream>

namespace fluxdb {
namespace ecs {

World::World(std::shared_ptr<ComponentStore> store, 
             std::shared_ptr<HistoryManager> history,
             std::shared_ptr<fluxdb::query::SubscriptionManager> pubsub) 
    : store_(store), history_(history), pubsub_(pubsub),
      codec_registry_(std::make_unique<delta::CodecRegistry>()),
      lod_manager_(std::make_unique<lod::LodManager>()) {
    // Al iniciar, registramos el arquetipo "vacío" (sin componentes)
    ArchetypeSignature empty_sig;
    archetypes_[empty_sig.to_ullong()] = std::make_unique<Archetype>(empty_sig, *store_);

    // Initialize Spatial Index (-10km to +10km)
    spatial_index_ = std::make_unique<fluxdb::spatial::SpatialIndex>(
        fluxdb::spatial::Bounds{-10000, -10000, -10000, 10000, 10000, 10000});

    // (#19) Grid sectorial: keyed por el MISMO grid que las posiciones
    // SectorPos (celda = SECTOR_SIZE); sin límites de mundo.
    sector_grid_ = std::make_unique<fluxdb::ecs::SpatialSectorGrid>();

    // Interest-Managed Spatial Pub/Sub (#9): el manager consulta candidatos
    // al índice espacial y resuelve posiciones vía el store del World.
    // Estos providers NO se invocan mientras el World sostiene su lock
    // (refresh_volumes/subscribe los llaman fuera de él).
    if (pubsub_) {
        pubsub_->set_entity_query_fn(
            [this](float cx, float cy, float cz, float r,
                   std::vector<fluxdb::query::SubscriptionManager::SpatialCandidate>& out) {
                if (pos_id_ == 255) return;
                if (sector_positions_) {
                    // (#19) Modo sector: consulta al grid sectorial y entrega
                    // coordenadas RELATIVAS al sector del centro del volumen
                    // (float exacto cerca del centro — sin jitter a 1e9+).
                    SectorPos center = SectorPos::from_world(cx, cy, cz);
                    std::vector<std::pair<Entity, SectorPos>> hits;
                    sector_grid_->query_range(center, r, hits);
                    for (const auto& [e, p] : hits) {
                        float wx, wy, wz;
                        p.to_world(wx, wy, wz);
                        out.push_back({e, wx, wy, wz});
                    }
                    return;
                }
                std::vector<uint32_t> ids;
                spatial_index_->query_range(cx, cy, cz, r, ids);
                for (uint32_t e : ids) {
                    size_t sz = 0;
                    const float* p = static_cast<const float*>(get_entity_component_data(e, pos_id_, sz));
                    if (!p || sz < 3 * sizeof(float)) continue;
                    out.push_back({e, p[0], p[1], p[2]});
                }
            });
        pubsub_->set_replicated_check_fn([this](uint32_t e) {
            return replicated_entities_.find(e) != replicated_entities_.end();
        });
    }
}

void World::set_position_component_id(ComponentID id) {
    pos_id_ = id;
    // (#19) El modo sector se activa si el componente de posición se registró
    // con el tamaño exacto de SectorPos (24 bytes). El resto de los tests y
    // sistemas siguen con el índice de mundo legacy (floats 3×32).
    sector_positions_ = (store_->get_info(id).size == sizeof(SectorPos));
}

void World::advance_tick() {
    version_.advance_tick();
    if (history_) {
        history_->advance_tick();
    }
    // (#1) Heurística hot/cold: decay de contadores solo con profiling activo.
    if (store_->access_profiling()) {
        for (const auto& [hash, arch] : archetypes_) {
            arch->decay_access_counts();
        }
    }
}

Entity World::spawn() {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    Entity ent = next_entity_++;
    
    ArchetypeSignature empty_sig;
    Archetype* arch = archetypes_[empty_sig.to_ullong()].get();
    
    size_t row = arch->add_entity(ent);
    entity_locations_[ent] = {arch, row};
    structural_events_.push_back({current_tick(), ent, true});

    return ent;
}

Entity World::spawn_with_id(Entity id) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(id);
    if (it != entity_locations_.end()) {
        return id; // idempotente: replay aplica spawns duplicados sin crear dupes
    }
    next_entity_ = std::max(next_entity_, id + 1);

    ArchetypeSignature empty_sig;
    Archetype* arch = archetypes_[empty_sig.to_ullong()].get();

    size_t row = arch->add_entity(id);
    entity_locations_[id] = {arch, row};
    structural_events_.push_back({current_tick(), id, true});

    return id;
}

void World::despawn(Entity entity) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(entity);
    if (it != entity_locations_.end()) {
        Archetype* arch = it->second.archetype;
        size_t row = it->second.row;
        
        Entity moved = arch->remove_entity(row);
        if (moved != UINT32_MAX) {
            entity_locations_[moved].row = row; // Update moved entity
        }
        
        entity_locations_.erase(it);
        structural_events_.push_back({current_tick(), entity, false});

        // Native Entity Relationship Graphs (#6): despawn limpia todas las
        // aristas del entity (como src y como dst), sellando el versionado.
        relations_.remove_all_relations(entity, current_tick());

        // Interest-Managed Spatial Pub/Sub (#9): limpieza de volumen y
        // marca de replicable.
        auto vit = entity_interest_subs_.find(entity);
        if (vit != entity_interest_subs_.end()) {
            if (pubsub_) pubsub_->unsubscribe(vit->second);
            entity_interest_subs_.erase(vit);
        }
        replicated_entities_.erase(entity);

        // (#10) El mundo la despawna: relevancia de los suscriptores y
        // despawn diferido en su próximo delta.
        if (pubsub_) pubsub_->notify_entity_despawned(entity);
    }
}

World::~World() = default;

Archetype* World::get_or_create_archetype(ArchetypeSignature sig) {
    unsigned long long hash = sig.to_ullong();
    auto it = archetypes_.find(hash);
    
    if (it == archetypes_.end()) {
        auto arch = std::make_unique<Archetype>(sig, *store_);
        Archetype* ptr = arch.get();
        archetypes_[hash] = std::move(arch);

        // Compiled Query Plans (#5): índice de firma de componentes +
        // invalidación selectiva — solo los planes que matchean esta firma
        // nueva se actualizan (los demás quedan intactos).
        for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
            if (sig.test(i)) {
                component_archetype_index_[static_cast<ComponentID>(i)].push_back(ptr);
            }
        }
        for (auto& plan : plans_) {
            if (plan->matches_archetype(sig)) {
                plan->add_archetype(ptr, *store_);
            }
        }

        return ptr;
    }
    
    return it->second.get();
}

void World::move_entity(Entity entity, Archetype* old_arch, size_t old_row, Archetype* new_arch) {
    size_t new_row = new_arch->add_entity(entity, current_tick());
    
    // Copiar componentes que existen en ambos arquetipos
    ArchetypeSignature old_sig = old_arch->get_signature();
    ArchetypeSignature new_sig = new_arch->get_signature();

    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (old_sig.test(i) && new_sig.test(i)) {
            ComponentID c = static_cast<ComponentID>(i);
            void* data = old_arch->get_component_data(old_row, c);
            // Migración estructural: el payload se copia SIN sellar como write
            // lógico, pero el tick sellado previo (#4) se PRESERVA — añadir un
            // componente nuevo no debe borrar el historial de versionado de
            // los existentes (for_each_changed/deltas lo necesitan).
            uint32_t preserved = old_arch->last_write_tick(old_row, c);
            new_arch->set_component_data(new_row, c, data, entity, nullptr, preserved);
        }
    }
    
    // Eliminar la entidad de su viejo arquetipo
    Entity moved = old_arch->remove_entity(old_row, current_tick());
    if (moved != UINT32_MAX) {
        entity_locations_[moved].row = old_row;
    }
    
    // Actualizar locación de la entidad movida
    entity_locations_[entity] = {new_arch, new_row};
}

void World::add_component(Entity entity, ComponentID comp_id, const void* data) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(entity);
    if (it == entity_locations_.end()) return; // Entity doesnt exist

    Archetype* old_arch = it->second.archetype;
    size_t old_row = it->second.row;
    
    ArchetypeSignature new_sig = old_arch->get_signature();
    
    // Si ya lo tiene, se sobreescribe
    if (new_sig.test(comp_id)) {
        if (pubsub_ && comp_id == pos_id_) {
            float old_p[3], new_p[3];
            if (sector_positions_) {
                // (#19) En modo sector, el componente es SectorPos (24 bytes).
                // Convertimos a world coords para notify_entity_moved.
                const SectorPos* osp = static_cast<const SectorPos*>(old_arch->get_component_data(old_row, comp_id));
                const SectorPos* nsp = static_cast<const SectorPos*>(data);
                osp->to_world(old_p[0], old_p[1], old_p[2]);
                nsp->to_world(new_p[0], new_p[1], new_p[2]);
            } else {
                const float* old_pos = reinterpret_cast<const float*>(old_arch->get_component_data(old_row, comp_id));
                const float* new_pos = reinterpret_cast<const float*>(data);
                old_p[0] = old_pos[0]; old_p[1] = old_pos[1]; old_p[2] = old_pos[2];
                new_p[0] = new_pos[0]; new_p[1] = new_pos[1]; new_p[2] = new_pos[2];
            }
            pubsub_->notify_entity_moved(entity, "", old_p[0], old_p[1], old_p[2], new_p[0], new_p[1], new_p[2]);

            // (#19) Actualiza el grid sectorial en re-add
            if (sector_positions_) {
                const SectorPos* nsp = static_cast<const SectorPos*>(data);
                sector_grid_->update(entity, *nsp);
            }

            // (#9) El volumen de interés sigue al owner: se traslada con él.
            if (interest_vol_id_ != 255) {
                auto vit = entity_interest_subs_.find(entity);
                if (vit != entity_interest_subs_.end()) {
                    const fluxdb::query::InterestVolume* cur = static_cast<const fluxdb::query::InterestVolume*>(
                        old_arch->get_component_data(old_row, interest_vol_id_));
                    if (cur) {
                        fluxdb::query::InterestVolume vol = *cur;
                        float dx = new_p[0] - old_p[0];
                        float dy = new_p[1] - old_p[1];
                        float dz = new_p[2] - old_p[2];
                        vol.cx += dx; vol.cy += dy; vol.cz += dz;
                        vol.max_x += dx; vol.max_y += dy; vol.max_z += dz;
                        pubsub_->update_volume(vit->second, vol);
                    }
                }
            }
        }

        old_arch->set_component_data(old_row, comp_id, data, entity, history_.get(), current_tick());
        return;
    }
    
    new_sig.set(comp_id);
    Archetype* new_arch = get_or_create_archetype(new_sig);
    
    move_entity(entity, old_arch, old_row, new_arch);
    
    // FIX: el data del componente nuevo se ignora en el move path.
    // Se escribe (sellando la versión) ahora que la entidad está en el nuevo arquetipo.
    size_t new_row = entity_locations_[entity].row;
    new_arch->set_component_data(new_row, comp_id, data, entity, history_.get(), current_tick());

    // Interest-Managed Spatial Pub/Sub (#9): el par Replicated +
    // InterestVolume conduce la relevancia de red automática.
    if (comp_id == interest_vol_id_ && pubsub_) {
        const fluxdb::query::InterestVolume* vol = static_cast<const fluxdb::query::InterestVolume*>(data);
        upsert_interest_volume(entity, *vol);
    }
    if (comp_id == replicated_id_) {
        replicated_entities_.insert(entity);
    }
    
    if (comp_id == pos_id_) {
        if (sector_positions_) {
            // (#19) Modo sector-relative: el grid espacial se alimenta con
            // SectorPos directamente (offsets locales exactos).
            const SectorPos* sp = reinterpret_cast<const SectorPos*>(data);
            sector_grid_->update(entity, *sp);

            if (pubsub_) {
                float wx, wy, wz;
                sp->to_world(wx, wy, wz);
                pubsub_->notify_entity_moved(entity, "", 0, 0, 0, wx, wy, wz);

                // (#9) El volumen de interés sigue al owner (world coords del
                // sector actual — solo se usa como centro de volúmenes).
                if (interest_vol_id_ != 255) {
                    auto vit = entity_interest_subs_.find(entity);
                    if (vit != entity_interest_subs_.end()) {
                        const fluxdb::query::InterestVolume* cur = static_cast<const fluxdb::query::InterestVolume*>(
                            new_arch->get_component_data(new_row, interest_vol_id_));
                        if (cur) {
                            fluxdb::query::InterestVolume vol = *cur;
                            vol.cx += wx; vol.cy += wy; vol.cz += wz;
                            vol.max_x += wx; vol.max_y += wy; vol.max_z += wz;
                            pubsub_->update_volume(vit->second, vol);
                        }
                    }
                }
            }
        } else {
            const float* p = reinterpret_cast<const float*>(data);
            spatial_index_->update_entity(entity, p[0], p[1], p[2]);
            
            if (pubsub_) {
                pubsub_->notify_entity_moved(entity, "", 0, 0, 0, p[0], p[1], p[2]);

                // (#9) El volumen de interés sigue al owner también en el
                // primer posicionamiento (vieja posición = 0,0,0).
                if (interest_vol_id_ != 255) {
                    auto vit = entity_interest_subs_.find(entity);
                    if (vit != entity_interest_subs_.end()) {
                        const fluxdb::query::InterestVolume* cur = static_cast<const fluxdb::query::InterestVolume*>(
                            new_arch->get_component_data(new_row, interest_vol_id_));
                        if (cur) {
                            fluxdb::query::InterestVolume vol = *cur;
                            vol.cx += p[0]; vol.cy += p[1]; vol.cz += p[2];
                            vol.max_x += p[0]; vol.max_y += p[1]; vol.max_z += p[2];
                            pubsub_->update_volume(vit->second, vol);
                        }
                    }
                }
            }
        }
    } else {
        // debug
        // std::cout << "[World] Added component " << (int)comp_id << " to entity " << entity << " (pos_id=" << (int)pos_id_ << ")" << std::endl;
    }
}

const SectorPos* World::sector_position(Entity entity) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    if (!sector_positions_) return nullptr;
    return sector_grid_->find(entity);
}

// ── Time-Travel Collision Queries (#13) ──

bool World::raycast(const fluxdb::physics::Ray& ray, float radius, float max_distance,
                    fluxdb::physics::RaycastHit& out) const {
    return raycast_historical(UINT64_MAX, ray, radius, max_distance, out);
}

bool World::raycast_historical(uint64_t tick, const fluxdb::physics::Ray& ray,
                               float radius, float max_distance,
                               fluxdb::physics::RaycastHit& out) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    if (pos_id_ == 255 || radius <= 0) return false;

    const ComponentStore* store = store_.get();
    size_t psz = store->get_info(pos_id_).size;
    if (psz < 3 * sizeof(float)) return false;

    float best = max_distance > 0 ? max_distance : std::numeric_limits<float>::max();
    bool any = false;

    for (Archetype* arch : canonical_archetypes()) {
        const Entity* ents = arch->get_entities_ptr();
        if (!ents) continue;
        ArchetypeSignature sig = arch->get_signature();
        if (!sig.test(pos_id_)) continue;
        const uint8_t* pos_base = static_cast<const uint8_t*>(arch->get_component_data(0, pos_id_));
        if (!pos_base) continue;
        size_t stride = store->get_info(pos_id_).size;
        for (size_t row = 0; row < arch->get_entity_count(); ++row) {
            const float* p = reinterpret_cast<const float*>(pos_base + row * stride);
            float px = p[0], py = p[1], pz = p[2];
            if (tick != UINT64_MAX && history_) {
                float h[3];
                if (history_->get_historical_state(tick, ents[row], pos_id_, h, sizeof(h))) {
                    px = h[0]; py = h[1]; pz = h[2];
                }
            }
            float d = ray.intersect_sphere(px, py, pz, radius * radius);
            if (d >= 0 && d < best) {
                best = d;
                out.entity = ents[row];
                out.distance = d;
                out.x = ray.ox + ray.dx * d;
                out.y = ray.oy + ray.dy * d;
                out.z = ray.oz + ray.dz * d;
                out.hit = true;
                any = true;
            }
        }
    }
    return any;
}

void World::query_volume_historical(uint64_t tick, const fluxdb::physics::VolumeQuery& vol,
                                    std::vector<uint32_t>& out) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    if (pos_id_ == 255) return;

    const ComponentStore* store = store_.get();
    size_t psz = store->get_info(pos_id_).size;
    if (psz < 3 * sizeof(float)) return;

    for (Archetype* arch : canonical_archetypes()) {
        const Entity* ents = arch->get_entities_ptr();
        if (!ents) continue;
        ArchetypeSignature sig = arch->get_signature();
        if (!sig.test(pos_id_)) continue;
        const uint8_t* pos_base = static_cast<const uint8_t*>(arch->get_component_data(0, pos_id_));
        if (!pos_base) continue;
        size_t stride = store->get_info(pos_id_).size;
        for (size_t row = 0; row < arch->get_entity_count(); ++row) {
            const float* p = reinterpret_cast<const float*>(pos_base + row * stride);
            float px = p[0], py = p[1], pz = p[2];
            if (tick != UINT64_MAX && history_) {
                float h[3];
                if (history_->get_historical_state(tick, ents[row], pos_id_, h, sizeof(h))) {
                    px = h[0]; py = h[1]; pz = h[2];
                }
            }
            bool inside = false;
            if (vol.shape == fluxdb::physics::VolumeShape2::SPHERE) {
                float ddx = px - vol.cx, ddy = py - vol.cy, ddz = pz - vol.cz;
                inside = (ddx * ddx + ddy * ddy + ddz * ddz) <= vol.r * vol.r;
            } else {
                inside = px >= vol.cx && px <= vol.max_x &&
                         py >= vol.cy && py <= vol.max_y &&
                         pz >= vol.cz && pz <= vol.max_z;
            }
            if (inside) out.push_back(ents[row]);
        }
    }
}

const void* World::get_entity_component_data(Entity entity, ComponentID comp_id, size_t& out_size) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(entity);
    if (it == entity_locations_.end()) return nullptr;

    Archetype* arch = it->second.archetype;
    ArchetypeSignature sig = arch->get_signature();
    
    if (!sig.test(comp_id)) return nullptr;
    
    out_size = store_->get_info(comp_id).size;
    return arch->get_component_data(it->second.row, comp_id);
}

bool World::entity_changed_since(Entity entity, ComponentID comp_id, uint64_t since_tick) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(entity);
    if (it == entity_locations_.end()) return false;

    Archetype* arch = it->second.archetype;
    return arch->last_write_tick(it->second.row, comp_id) > since_tick;
}

uint32_t World::entity_last_write_tick(Entity entity, ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_locations_.find(entity);
    if (it == entity_locations_.end()) return 0;

    Archetype* arch = it->second.archetype;
    return arch->last_write_tick(it->second.row, comp_id);
}

// ── Native Entity Relationship Graphs (#6) ──

void World::add_relation(Entity src, RelationKind kind, Entity dst, RelationPayload payload) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    if (entity_locations_.find(src) == entity_locations_.end()) return;
    if (entity_locations_.find(dst) == entity_locations_.end()) return;
    relations_.add_relation(src, kind, dst, payload, current_tick());
}

bool World::remove_relation(Entity src, RelationKind kind, Entity dst) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.remove_relation(src, kind, dst, current_tick());
}

bool World::has_relation(Entity src, RelationKind kind, Entity dst) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.has_relation(src, kind, dst);
}

bool World::get_relation(Entity src, RelationKind kind, Entity dst, RelationPayload& out) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.get_relation(src, kind, dst, out);
}

size_t World::outgoing_degree(Entity src, RelationKind kind) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.outgoing_degree(src, kind);
}

size_t World::incoming_degree(Entity target, RelationKind kind) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.incoming_degree(target, kind);
}

bool World::relation_kind_changed_since(RelationKind kind, uint64_t since_tick) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.kind_changed_since(kind, since_tick);
}

uint32_t World::relation_last_write_tick(RelationKind kind, Entity src) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    return relations_.last_write_tick(kind, src);
}

QueryHandle World::create_query(std::vector<ComponentID> required) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);

    ArchetypeSignature sig;
    for (ComponentID c : required) {
        sig.set(c);
    }
    unsigned long long key = sig.to_ullong();

    auto cached = query_by_signature_.find(key);
    if (cached != query_by_signature_.end()) {
        return cached->second; // dedupe: misma firma, mismo plan
    }

    auto plan = std::make_unique<QueryPlan>(required, *store_);
    for (Archetype* arch : canonical_archetypes()) {
        if (plan->matches_archetype(arch->get_signature())) {
            plan->add_archetype(arch, *store_);
        }
    }

    QueryHandle handle = static_cast<QueryHandle>(plans_.size());
    plans_.push_back(std::move(plan));
    query_by_signature_[key] = handle;
    return handle;
}

const QueryPlan* World::get_query_plan(QueryHandle handle) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    if (handle >= plans_.size()) {
        return nullptr;
    }
    return plans_[handle].get();
}

bool World::remove_archetype(ArchetypeSignature sig) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);

    // El arquetipo vacío es estructural (spawn/spawn_with_id lo usan): no se toca.
    if (sig.none()) {
        return false;
    }

    unsigned long long hash = sig.to_ullong();
    auto it = archetypes_.find(hash);
    if (it == archetypes_.end()) {
        return false;
    }

    Archetype* arch = it->second.get();
    if (arch->get_entity_count() > 0) {
        return false; // no se remueven arquetipos con entidades vivas
    }

    // Compiled Query Plans (#5): invalidación por remoción — índice
    // componente→arquetipos y los planes que matcheaban este arquetipo.
    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (sig.test(i)) {
            auto& list = component_archetype_index_[static_cast<ComponentID>(i)];
            list.erase(std::remove(list.begin(), list.end(), arch), list.end());
        }
    }
    for (auto& plan : plans_) {
        plan->remove_archetype(arch);
    }

    archetypes_.erase(it);
    return true;
}

// ── Hot-Reload Component Schemas Without World Reset (#30) ──

size_t World::hot_reload_component(ComponentID comp_id, size_t new_size,
                                   const Archetype::FieldMap* field_map, size_t num_fields,
                                   uint8_t default_fill) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);

    // Migra el layout en todos los arquetipos que contienen el componente.
    size_t migrated = 0;
    for (auto& [hash, up] : archetypes_) {
        Archetype* arch = up.get();
        if (!arch->has_component(comp_id)) continue;
        migrated += arch->migrate_component_layout(comp_id, new_size, field_map, num_fields, default_fill);
    }

    // Los writes futuros (set_component_data) y los arquetipos nuevos usan el
    // nuevo stride del store.
    if (migrated > 0) {
        store_->set_component_size(comp_id, new_size);
    }
    return migrated;
}


uint32_t World::interest_subscription(Entity e) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    auto it = entity_interest_subs_.find(e);
    return it == entity_interest_subs_.end() ? 0 : it->second;
}

void World::flush_interest_events() {
    if (!pubsub_) return;
    pubsub_->refresh_volumes();
    pubsub_->flush_events();
}

void World::upsert_interest_volume(Entity e, const fluxdb::query::InterestVolume& vol) {
    if (!pubsub_) return;
    auto it = entity_interest_subs_.find(e);
    if (it != entity_interest_subs_.end()) {
        pubsub_->update_volume(it->second, vol);
        return;
    }
    uint32_t sid = pubsub_->subscribe_volume("", vol, nullptr);
    entity_interest_subs_[e] = sid;
}

// ── Unified Delta Engine (#7) ──

void World::advance_to(uint64_t tick) {
    version_.advance_to(tick);
    if (history_) {
        history_->advance_to(tick);
    }
}

delta::CodecRegistry& World::codec_registry() const {
    return *codec_registry_;
}

void World::set_codec(ComponentID comp_id, delta::CodecID codec) {
    codec_registry_->set_codec(comp_id, codec);
}

const std::vector<World::StructuralEvent>& World::structural_events() const {
    return structural_events_;
}

void World::prune_structural_events(uint64_t before_tick) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    structural_events_.erase(
        std::remove_if(structural_events_.begin(), structural_events_.end(),
            [&](const StructuralEvent& e) { return e.tick <= before_tick; }),
        structural_events_.end());

    // Relaciones (#6): las tumbas de aristas removidas en o antes del tick
    // ya no deben propagarse en los diffs (#7).
    relations_.prune_tombstones(before_tick);
}

bool World::save_incremental(const std::string& path) {
    delta::ReplayRecorder recorder;
    if (!recorder.open(path)) return false;
    recorder.begin_recording(*this);
    recorder.close();
    return true;
}

bool World::save_incremental(const std::string& path, const std::string& from_existing) {
    // Carga el archivo existente en un world temporal, escribe el estado
    // plegado como base + deltas desde ese momento, SIN duplicar la base.
    delta::ReplayPlayer player;
    if (!player.open(from_existing)) return false;

    auto tmp_store = std::make_shared<ComponentStore>();
    World tmp(tmp_store, std::make_shared<HistoryManager>(64));
    player.load_components(tmp);
    while (player.step(tmp)) {}
    uint64_t base_tick = tmp.current_tick();

    if (current_tick() <= base_tick) {
        return save_incremental(path);
    }

    // Compactar el archivo viejo.
    std::string folded = from_existing + ".folded";
    if (!delta::fold_replay_file(from_existing, folded)) return false;

    // Reabrir el plegado en un world y capturarlo como la nueva base.
    delta::ReplayPlayer folded_player;
    if (!folded_player.open(folded)) { std::remove(folded.c_str()); return false; }
    auto base_store = std::make_shared<ComponentStore>();
    World base_world(base_store, std::make_shared<HistoryManager>(64));
    folded_player.load_components(base_world);
    while (folded_player.step(base_world)) {}
    std::remove(folded.c_str());

    // Escribir archivo con la base plegada + deltas nuevos.
    delta::ReplayRecorder recorder;
    if (!recorder.open(path)) return false;
    recorder.begin_recording(base_world);
    recorder.record_tick(*this);
    recorder.close();
    return true;
}

bool World::compact_save(const std::string& path) {
    // Folding de la cadena de deltas en el snapshot base (#7).
    std::string tmp = path + ".compact";
    if (!delta::fold_replay_file(path, tmp)) return false;
    if (std::remove(path.c_str()) != 0) return false;
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    return true;
}

bool World::load_from_replay(const std::string& path) {
    delta::ReplayPlayer player;
    if (!player.open(path)) return false;
    player.load_components(*this);
    while (player.step(*this)) {
    }
    return player.finished();
}

// ── First-Class Rollback Netcode (#8) ──

void World::clear_all() {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);

    archetypes_.clear();
    ArchetypeSignature empty_sig;
    archetypes_[empty_sig.to_ullong()] = std::make_unique<Archetype>(empty_sig, *store_);

    entity_locations_.clear();
    structural_events_.clear();
    component_archetype_index_.clear();
    relations_.clear(); // (#6): sin aristas colgantes tras el reset

    // Los planes cacheados apuntan a arquetipos destruidos: se invalidan
    // (create_query los recompila bajo demanda).
    plans_.clear();
    query_by_signature_.clear();

    // Las entidades restauradas se reinsertan vía spawn_with_id (pos_id_
    // desconocido en restore: no se toca el spatial index).
    spatial_index_ = std::make_unique<fluxdb::spatial::SpatialIndex>(
        fluxdb::spatial::Bounds{-10000, -10000, -10000, 10000, 10000, 10000});
    // (#19) El grid sectorial también se reinicia (estado derivado).
    sector_grid_ = std::make_unique<fluxdb::ecs::SpatialSectorGrid>();
}

bool World::rollback_to(uint64_t tick) {
    if (!rollback_ring_) return false;
    const rollback::SnapshotRingBuffer& ring = *rollback_ring_;
    if (!ring.has_base() || !ring.covers_tick(tick)) return false;

    // (#8) Fast path COW: restaurar la base repuntando las páginas de chunks
    // a la versión histórica (sin copiar payloads), luego re-aplicar los
    // deltas hacia adelante. El reloj se fija al end_tick de cada delta
    // ANTES de aplicarlo para que los stamps de versión (#4) coincidan con
    // el tick original y los deltas no consecutivos (evictados del ring)
    // sigan siendo exactos.
    if (ring.base_pages().captured) {
        restore_chunk_pages(ring.base_pages());
    } else {
        ring.base().restore(*this);
    }
    advance_to(ring.base_tick());
    for (const auto& d : ring.deltas()) {
        if (d.end_tick() > tick) break;
        advance_to(d.end_tick());
        d.apply(*this);
    }
    return true;
}

bool World::resimulate(uint64_t from_tick, uint64_t to_tick) {
    if (!rollback_to(from_tick)) return false;

    const rollback::SnapshotRingBuffer& ring = *rollback_ring_;
    for (const auto& d : ring.deltas()) {
        if (d.end_tick() <= from_tick) continue;
        if (d.end_tick() > to_tick) break;
        advance_to(d.end_tick());
        d.apply(*this);
    }
    return true;
}

bool World::resimulate(uint64_t from_tick, uint64_t to_tick, const std::vector<rollback::ExternalInput>& inputs) {
    if (!rollback_to(from_tick)) return false;

    const rollback::SnapshotRingBuffer& ring = *rollback_ring_;
    // Por cada tick del rango: delta grabado (si existe) y luego los inputs
    // de ese tick (corrección gana). Inputs fuera del rango se ignoran.
    for (uint64_t t = from_tick + 1; t <= to_tick; ++t) {
        for (const auto& d : ring.deltas()) {
            if (d.end_tick() == t) {
                advance_to(t);
                d.apply(*this);
                break;
            }
        }
        for (const auto& in : inputs) {
            if (in.tick == t) {
                advance_to(t);
                apply_external_input(in);
            }
        }
    }
    return true;
}

void World::apply_external_input(const rollback::ExternalInput& in) {
    switch (in.op) {
    case rollback::InputOp::SET_COMPONENT:
        if (!in.data.empty()) {
            add_component(in.entity, in.comp_id, in.data.data());
        }
        break;
    case rollback::InputOp::ADD_RELATION:
        add_relation(in.entity, in.kind, in.dst, in.payload);
        break;
    case rollback::InputOp::REMOVE_RELATION:
        remove_relation(in.entity, in.kind, in.dst);
        break;
    }
}

// ── COW structural snapshot (#8) ──────────────────────────────

void World::capture_chunk_pages(ChunkPageSnapshot& out) const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    out = ChunkPageSnapshot{};
    out.tick = current_tick();
    out.captured = true;
    for (const auto& [hash, arch] : archetypes_) {
        arch->snapshot_pages(out.archetypes[hash]);
    }
}

void World::restore_chunk_pages(const ChunkPageSnapshot& in) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);

    archetypes_.clear();
    entity_locations_.clear();
    structural_events_.clear();
    component_archetype_index_.clear();

    // Los planes cacheados apuntan a arquetipos destruidos: se invalidan
    // (create_query los recompila bajo demanda).
    plans_.clear();
    query_by_signature_.clear();
    relations_.clear(); // (#6): sin aristas colgantes tras el reset

    // Sin información de posiciones en el snapshot: índice espacial vacío.
    spatial_index_ = std::make_unique<fluxdb::spatial::SpatialIndex>(
        fluxdb::spatial::Bounds{-10000, -10000, -10000, 10000, 10000, 10000});

    for (const auto& [hash, state] : in.archetypes) {
        ArchetypeSignature sig(static_cast<unsigned long long>(hash));
        auto arch = std::make_unique<Archetype>(sig, *store_);
        arch->restore_pages(state);
        Archetype* ptr = arch.get();
        archetypes_[hash] = std::move(arch);

        for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
            if (sig.test(i)) {
                component_archetype_index_[static_cast<ComponentID>(i)].push_back(ptr);
            }
        }

        // Re-indexar ubicaciones + re-sellar eventos estructurales al tick
        // del snapshot (equivalente a re-aplicar los SPAWN del DeltaSet).
        for (size_t row = 0; row < state.num_entities; ++row) {
            Entity e = state.entities[row];
            entity_locations_[e] = {ptr, row};
            structural_events_.push_back({in.tick, e, true});
        }
    }
}

// ── Bandwidth-Aware Component LOD (#10) ───────────────────────

void World::set_component_lod(ComponentID comp_id, const std::vector<lod::LODRule>& rules) {
    lod_manager_->set_component_rules(comp_id, rules);
}

delta::DeltaSet World::build_subscriber_delta(uint32_t sub_id, uint64_t since_tick, bool include_cold_tiers) const {
    delta::DeltaSet d(since_tick);
    if (!pubsub_) return d;

    const std::unordered_set<uint32_t>& relevant = pubsub_->relevant_entities(sub_id);
    float vx = 0, vy = 0, vz = 0;
    bool has_vol = pubsub_->volume_center(sub_id, vx, vy, vz);
    uint64_t tick = current_tick();

    std::shared_lock<std::shared_mutex> lock(world_mutex_);

    // 1) Spawns/despawns por diff de relevancia del suscriptor (#10):
    // enter → SPAWN (el suscriptor aún no la conoce), leave → DESPAWN (la
    // limpió el volumen o se despawnó en el mundo, notify_entity_despawned).
    std::vector<uint32_t> enter, leave;
    pubsub_->replication_diff(sub_id, enter, leave);
    for (uint32_t e : leave) d.add_despawn(e);
    for (uint32_t e : enter) d.add_spawn(e);

    // 2) Componentes con LOD por (entity, subscriber): tier por distancia al
    // centro del volumen de interés + frecuencia + cuantización del tier.
    for (uint32_t e : relevant) {
        auto loc_it = entity_locations_.find(e);
        if (loc_it == entity_locations_.end()) continue;
        Archetype* arch = loc_it->second.archetype; // el puntero es const, el pointee no
        size_t row = loc_it->second.row;

        float dist = 0.0f;
        if (has_vol && pos_id_ != 255) {
            if (sector_positions_) {
                // (#19) Distancia sector-exacta sin materializar floats de
                // mundo (el comp de posición es SectorPos en este modo).
                const SectorPos* sp = static_cast<const SectorPos*>(arch->get_component_data(row, pos_id_));
                if (sp) {
                    dist = sp->distance_to(SectorPos::from_world(vx, vy, vz));
                }
            } else {
                size_t psz = store_->get_info(pos_id_).size;
                const float* p = static_cast<const float*>(arch->get_component_data(row, pos_id_));
                if (p && psz >= 3 * sizeof(float)) {
                    float dx = p[0] - vx, dy = p[1] - vy, dz = p[2] - vz;
                    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
        }

        ArchetypeSignature sig = arch->get_signature();
        for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
            if (!sig.test(i)) continue;
            ComponentID comp = static_cast<ComponentID>(i);

        lod::Tier tier = lod_manager_->tier_for(comp, dist);
        if (tier == lod::Tier::NONE) continue;

        // (#1) Serialización de red solo hot/replicated: los componentes
        // COLD se saltan por defecto (snapshot de red sin caminar sus arrays);
        // opt-in con include_cold_tiers=true (comportamiento previo).
        if (!include_cold_tiers && store_->get_tier(comp) == ComponentTier::COLD) continue;

        uint32_t last_write = arch->last_write_tick(row, comp);
            if (!lod_manager_->should_update(comp, e, tier, last_write, tick)) {
                continue; // frecuencia del tier: aún no toca
            }

            size_t csz = store_->get_info(comp).size;
            std::vector<uint8_t> bytes(csz);
            std::memcpy(bytes.data(), arch->get_component_data(row, comp), csz);
            lod_manager_->quantize(comp, tier, bytes.data(), bytes.size());

            d.add_update(e, comp, bytes.data(), bytes.size());
            lod_manager_->mark_sent(comp, e, tier, tick);
        }
    }
    return d;
}

// ── Deterministic Lockstep Mode (#11) ────────────────────────

std::vector<Archetype*> World::canonical_archetypes() const {
    std::vector<Archetype*> out;
    out.reserve(archetypes_.size());
    for (const auto& [hash, arch] : archetypes_) {
        out.push_back(arch.get());
    }
    // Orden canónico: firma ascendente (bitset → entero). Independiente del
    // orden de inserción y del unordered_map.
    std::sort(out.begin(), out.end(), [](const Archetype* a, const Archetype* b) {
        return a->get_signature().to_ullong() < b->get_signature().to_ullong();
    });
    return out;
}

void World::enable_determinism_lock() {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    determinism_locked_ = true;
}

void World::apply_inputs(const std::vector<rollback::ExternalInput>& inputs) {
    for (const auto& in : inputs) {
        apply_external_input(in);
    }
}

uint64_t World::state_hash() const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);

    // FNV-1a 64.
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&h](const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    auto mix_u32 = [&mix](uint32_t v) { mix(&v, sizeof(v)); };
    auto mix_u64 = [&mix](uint64_t v) { mix(&v, sizeof(v)); };
    auto mix_i32 = [&mix_u32](int32_t v) { mix_u32(static_cast<uint32_t>(v)); };

    mix_u64(current_tick());

    // 1) Arquetipos en orden canónico: firma, tamaño, y por entidad
    //    ORDENADA (el layout de filas es interno; el hash es del contenido).
    for (Archetype* arch : canonical_archetypes()) {
        mix_u64(arch->get_signature().to_ullong());

        struct Row {
            Entity e;
            std::vector<std::pair<ComponentID, std::pair<uint32_t, std::vector<uint8_t>>>> comps;
            bool operator<(const Row& o) const { return e < o.e; }
        };
        std::vector<Row> rows;
        rows.reserve(arch->get_entity_count());

        const Entity* ents = arch->get_entities_ptr();
        const size_t n = arch->get_entity_count();
        ArchetypeSignature sig = arch->get_signature();
        for (size_t row = 0; row < n; ++row) {
            Row r;
            r.e = ents[row];
            for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
                if (!sig.test(i)) continue;
                ComponentID comp = static_cast<ComponentID>(i);
                const size_t csz = store_->get_info(comp).size;
                const uint8_t* data = static_cast<const uint8_t*>(arch->get_component_data(row, comp));
                std::vector<uint8_t> bytes(data, data + csz);
                r.comps.push_back({comp, {arch->last_write_tick(row, comp), std::move(bytes)}});
            }
            std::sort(r.comps.begin(), r.comps.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            rows.push_back(std::move(r));
        }
        std::sort(rows.begin(), rows.end());

        mix_u32(static_cast<uint32_t>(rows.size()));
        for (const Row& r : rows) {
            mix_u32(r.e);
            for (const auto& [comp, w] : r.comps) {
                mix(&comp, 1);
                mix_u32(w.first);
                mix(w.second.data(), w.second.size());
            }
        }
    }

    // 2) Relaciones: aristas ordenadas por (src, kind, dst) — la iteración
    //    interna del grafo es no-determinista, así que se canoniciza.
    struct Edge {
        uint32_t src, dst;
        uint8_t kind;
        uint64_t payload;
        bool operator<(const Edge& o) const {
            if (src != o.src) return src < o.src;
            if (kind != o.kind) return kind < o.kind;
            return dst < o.dst;
        }
    };
    std::vector<Edge> edges;
    relations_.for_each_edge([&](uint32_t src, RelationKind kind, uint32_t dst,
                                  const RelationPayload& payload) {
        edges.push_back({src, dst, static_cast<uint8_t>(kind), payload.raw});
    });
    std::sort(edges.begin(), edges.end());
    mix_u64(static_cast<uint64_t>(edges.size()));
    for (const Edge& e : edges) {
        mix_u32(e.src);
        mix_u32(e.dst);
        mix(&e.kind, 1);
        mix_u64(e.payload);
    }

    // 3) Eventos estructurales (spawn/despawn) ORDENADOS por (tick, entity):
    //    el log acumulado depende del orden de operaciones; el hash es del
    //    contenido (mismo estado → mismo hash).
    std::vector<StructuralEvent> events = structural_events_;
    std::sort(events.begin(), events.end(), [](const StructuralEvent& a, const StructuralEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.spawned < b.spawned;
    });
    mix_u64(static_cast<uint64_t>(events.size()));
    for (const StructuralEvent& ev : events) {
        mix_u64(ev.tick);
        mix_u32(ev.entity);
        mix(&ev.spawned, 1);
    }

    // NOTA: el índice espacial, el pub/sub, los planes de query y los
    // watermarks del LOD son estado DERIVADO (cache): no participan del
    // hash. Los sistemas lockstep deben leer solo el estado ECS.
    return h;
}

// ── Hot/Cold Archetype Splitting (#1) ────────────────────────

std::vector<World::TierStats> World::tier_stats() const {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    std::vector<TierStats> out;
    out.reserve(store_->count());
    for (size_t i = 0; i < store_->count(); ++i) {
        ComponentID id = static_cast<ComponentID>(i);
        TierStats st;
        st.comp_id = id;
        st.tier = store_->get_tier(id);
        st.accesses = 0;
        st.pages = 0;
        st.bytes = 0;
        for (const auto& [hash, arch] : archetypes_) {
            st.accesses += arch->access_count(id);
            st.pages += arch->page_count(id);
        }
        st.bytes = st.pages * Archetype::PAGE_ROWS * store_->get_info(id).size;
        out.push_back(st);
    }
    return out;
}

size_t World::reclassify_components(uint64_t hot_threshold, uint64_t cold_threshold) {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    size_t changed = 0;
    for (size_t i = 0; i < store_->count(); ++i) {
        ComponentID id = static_cast<ComponentID>(i);
        uint64_t accesses = 0;
        for (const auto& [hash, arch] : archetypes_) {
            accesses += arch->access_count(id);
        }
        ComponentTier tier;
        if (accesses >= hot_threshold) {
            tier = ComponentTier::HOT;
        } else if (accesses <= cold_threshold) {
            tier = ComponentTier::COLD;
        } else {
            tier = ComponentTier::WARM;
        }
        if (store_->get_tier(id) != tier) {
            store_->set_tier(id, tier);
            ++changed;
        }
    }
    return changed;
}

size_t World::prefetch_tiers(uint8_t tier_mask) {
    std::shared_lock<std::shared_mutex> lock(world_mutex_);
    size_t touched = 0;
    for (Archetype* arch : canonical_archetypes()) {
        touched += arch->prefetch_components(arch->components_in_tier(tier_mask));
    }
    return touched;
}

} // namespace ecs
} // namespace fluxdb
