#include "../headers/ecs.h"
#include <mutex>
#include <shared_mutex>
#include <iostream>

namespace fluxdb {
namespace ecs {

World::World(std::shared_ptr<ComponentStore> store, 
             std::shared_ptr<HistoryManager> history,
             std::shared_ptr<fluxdb::query::SubscriptionManager> pubsub) 
    : store_(store), history_(history), pubsub_(pubsub) {
    // Al iniciar, registramos el arquetipo "vacío" (sin componentes)
    ArchetypeSignature empty_sig;
    archetypes_[empty_sig.to_ullong()] = std::make_unique<Archetype>(empty_sig, *store_);

    // Initialize Spatial Index (-10km to +10km)
    spatial_index_ = std::make_unique<veldradb::spatial::SpatialIndex>(
        veldradb::spatial::Bounds{-10000, -10000, -10000, 10000, 10000, 10000});
}

Entity World::spawn() {
    std::unique_lock<std::shared_mutex> lock(world_mutex_);
    Entity ent = next_entity_++;
    
    ArchetypeSignature empty_sig;
    Archetype* arch = archetypes_[empty_sig.to_ullong()].get();
    
    size_t row = arch->add_entity(ent);
    entity_locations_[ent] = {arch, row};

    return ent;
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
    }
}

Archetype* World::get_or_create_archetype(ArchetypeSignature sig) {
    unsigned long long hash = sig.to_ullong();
    auto it = archetypes_.find(hash);
    
    if (it == archetypes_.end()) {
        auto arch = std::make_unique<Archetype>(sig, *store_);
        Archetype* ptr = arch.get();
        archetypes_[hash] = std::move(arch);
        return ptr;
    }
    
    return it->second.get();
}

void World::move_entity(Entity entity, Archetype* old_arch, size_t old_row, Archetype* new_arch) {
    size_t new_row = new_arch->add_entity(entity);
    
    // Copiar componentes que existen en ambos arquetipos
    ArchetypeSignature old_sig = old_arch->get_signature();
    ArchetypeSignature new_sig = new_arch->get_signature();

    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (old_sig.test(i) && new_sig.test(i)) {
            ComponentID c = static_cast<ComponentID>(i);
            void* data = old_arch->get_component_data(old_row, c);
            new_arch->set_component_data(new_row, c, data, entity, nullptr);
        }
    }
    
    // Eliminar la entidad de su viejo arquetipo
    Entity moved = old_arch->remove_entity(old_row);
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
            const float* old_pos = reinterpret_cast<const float*>(old_arch->get_component_data(old_row, comp_id));
            const float* new_pos = reinterpret_cast<const float*>(data);
            pubsub_->notify_entity_moved(entity, "", old_pos[0], old_pos[1], old_pos[2], new_pos[0], new_pos[1], new_pos[2]);
        }
    
        old_arch->set_component_data(old_row, comp_id, data, entity, history_.get());
        return;
    }
    
    new_sig.set(comp_id);
    Archetype* new_arch = get_or_create_archetype(new_sig);
    
    move_entity(entity, old_arch, old_row, new_arch);
    
    if (comp_id == pos_id_) {
        const float* p = reinterpret_cast<const float*>(data);
        spatial_index_->update_entity(entity, p[0], p[1], p[2]);
        
        if (pubsub_) {
            pubsub_->notify_entity_moved(entity, "", 0, 0, 0, p[0], p[1], p[2]);
        }
    } else {
        // debug
        // std::cout << "[World] Added component " << (int)comp_id << " to entity " << entity << " (pos_id=" << (int)pos_id_ << ")" << std::endl;
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

} // namespace ecs
} // namespace fluxdb
