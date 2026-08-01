#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include <shared_mutex>
#include <utility>
#include "history.h"
#include "pubsub.h"
#include "spatial_index.h"
#include "versioning.h"
#include "relations.h"
#include "delta_codec.h"
#include "lod.h"
#include "sector_pos.h"
#include "physics.h"

namespace fluxdb {

// Unified Delta Engine (#7) — definido en core/ecs/delta_set.cpp
namespace delta {
class CodecRegistry;
class DeltaSet;
}

// First-Class Rollback Netcode (#8) — definido en core/ecs/rollback.cpp
namespace rollback {
class SnapshotRingBuffer;
struct ExternalInput;
}

namespace ecs {

using Entity = uint32_t;
using ComponentID = uint8_t;
using QueryHandle = uint32_t;
constexpr size_t MAX_COMPONENTS = 64;
using ArchetypeSignature = std::bitset<MAX_COMPONENTS>;

// Compiled Query Plans (#5) — definido en core/ecs/query_plans.cpp
class QueryPlan;

// ── Hot/Cold Archetype Splitting (#1) ─────────────────────────
// Clasificación de componentes por FRECUENCIA DE ACCESO. Cada componente
// vive en su propio array de páginas por arquetipo (SoA por componente);
// el tier determina qué arrays tocan las iteraciones/prefetchs/serialización
// de red: una query hot-only nunca arrastra las líneas de caché de los
// componentes cold. La clasificación es metadato (se consulta al store en
// tiempo de iteración → reclassify es inmediato, sin mover datos).
enum class ComponentTier : uint8_t {
    HOT = 0,  // tocado cada frame (p.ej. Transform)
    WARM = 1, // tocado con frecuencia media (p.ej. Health)
    COLD = 2  // tocado rara vez (p.ej. QuestFlags)
};
constexpr uint8_t TIER_MASK_HOT  = 0b001;
constexpr uint8_t TIER_MASK_WARM = 0b010;
constexpr uint8_t TIER_MASK_COLD = 0b100;
constexpr uint8_t TIER_MASK_ALL  = 0b111;
constexpr size_t COMPONENT_TIER_COUNT = 3;

inline constexpr uint8_t tier_mask(ComponentTier t) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(t));
}
inline constexpr bool tier_in_mask(ComponentTier t, uint8_t mask) {
    return (mask & tier_mask(t)) != 0;
}

// Definición de información sobre un componente
struct ComponentInfo {
    ComponentID id;
    std::string name;
    size_t size;
    ComponentTier tier = ComponentTier::WARM; // (#1) clasificación hot/warm/cold
};

// ── COW structural snapshot (#8) ──────────────────────────────
// El snapshot de rollback NO copia los datos de componentes: comparte las
// páginas de chunks del storage (copy-on-write). Un write posterior a una
// página compartida la clona; "rollback" solo vuelve a apuntar los punteros
// activos a la versión histórica. Los metadatos estructurales (entities y
// ticks de versión) se copian: 8 bytes por entidad, el volumen pesado
// (los payloads) nunca se duplica salvo que se escriba encima.
struct ChunkPageSnapshot {
    struct ArchetypeState {
        std::vector<Entity> entities;
        uint32_t num_entities = 0;
        std::unordered_map<ComponentID, std::vector<uint32_t>> last_write_ticks;
        // Páginas COMPARTIDAS: componente -> chunk -> buffer (PAGE_ROWS filas).
        std::unordered_map<ComponentID, std::vector<std::shared_ptr<uint8_t[]>>> pages;
    };

    std::unordered_map<unsigned long long, ArchetypeState> archetypes;
    uint64_t tick = 0;
    bool captured = false;
};

// Almacén y registro central de Tipos de Componentes
class ComponentStore {
public:
    ComponentStore() = default;
    
    ComponentID register_component(const std::string& name, size_t size);
    ComponentID register_component(const std::string& name, size_t size, ComponentTier tier);
    ComponentID get_id(const std::string& name) const;
    const ComponentInfo& get_info(ComponentID id) const;
    size_t count() const { return components_.size(); }

    // ── Hot/Cold Archetype Splitting (#1) ──

    // Clasificación (compile-time / por registro). `reclassify_components`
    // del World la sobreescribe con la heurística de acceso en runtime.
    void set_tier(ComponentID id, ComponentTier tier);
    ComponentTier get_tier(ComponentID id) const;
    ComponentTier get_tier(const std::string& name) const;

    // Contadores de acceso: solo se incrementan con profiling activo
    // (overhead cero por defecto). El flag vive en el store (compartido con
    // los arquetipos), no requiere propagación.
    void set_access_profiling(bool on) { access_profiling_ = on; }
    bool access_profiling() const { return access_profiling_; }

    // ── Hot-Reload Component Schemas (#30) ──
    // Actualiza el tamaño (stride) del componente en el registro. Los
    // arquetipos ya existentes se migran vía Archetype::migrate_component_layout;
    // los que se creen DESPUÉS usan este nuevo tamaño. Devuelve false si no existe.
    bool set_component_size(ComponentID id, size_t size) {
        if (id >= components_.size()) return false;
        components_[id].size = size;
        return true;
    }

private:
    std::vector<ComponentInfo> components_;
    std::unordered_map<std::string, ComponentID> name_to_id_;
    bool access_profiling_ = false; // (#1) heurística de runtime
};

// Patrón Data-Oriented Design (SoA).
// Almacena datos contiguos de entidades que comparten la misma firma.
class Archetype {
public:
    // (#8) COW: una página cubre ChunkedDirtyTracker::CHUNK_SIZE filas.
    static constexpr size_t PAGE_ROWS = ChunkedDirtyTracker::CHUNK_SIZE;

    Archetype(ArchetypeSignature sig, const ComponentStore& store);
    
    ArchetypeSignature get_signature() const { return signature_; }
    size_t get_entity_count() const { return num_entities_; }
    
    // Añade una entidad vacía al final y devuelve su "fila" (índice).
    // `tick` marca el ring grueso (cambio estructural del array).
    size_t add_entity(Entity entity, uint64_t tick = 0);
    
    // Elimina la entidad en la 'fila' (swap con el último y pop)
    // Devuelve la Entity que fue movida (para actualizar su row) o UINT32_MAX si no movió a nadie
    Entity remove_entity(size_t row, uint64_t tick = 0);
    
    // Array de bytes para el ComponentID solicitado (puntero a la fila 0 de
    // la página del chunk 0). NO es contiguo entre chunks: usa
    // get_component_data(row, comp_id) para acceso por fila.
    uint8_t* get_component_array(ComponentID comp_id) {
        return static_cast<uint8_t*>(get_component_data(0, comp_id));
    }
    
    // Escribe datos RAW para el componente en la fila especificada.
    // `tick` > 0 sella el write con la versión actual (per-entity + coarse ring).
    // `tick` == 0 copia sin sellar (p.ej. migración estructural entre arquetipos).
    void set_component_data(size_t row, ComponentID comp_id, const void* data, Entity entity, HistoryManager* history, uint64_t tick);

    // Lee datos RAW
    void* get_component_data(size_t row, ComponentID comp_id);

    // ── Temporal Component Versioning (#4) ──

    bool has_component(ComponentID comp_id) const;

    // Coarse O(1): ¿hubo algún write a este array de componentes después de `since_tick`?
    bool has_any_write_since(ComponentID comp_id, uint64_t since_tick) const;

    // Chunk-granular (#4): O(1) — ¿el chunk `chunk_idx` (rango de filas de
    // ChunkedDirtyTracker::CHUNK_SIZE) tuvo writes después de `since_tick`?
    bool chunk_has_writes_since(ComponentID comp_id, size_t chunk_idx, uint64_t since_tick) const;

    // Número de chunks cubiertos por las filas actuales del array.
    size_t chunk_count(ComponentID comp_id) const;

    // Fine-grained: tick del último write de esta entidad (0 = nunca).
    uint32_t last_write_tick(size_t row, ComponentID comp_id) const;

    // ── COW structural snapshot (#8) ──

    // Copia los metadatos estructurales (entities/ticks) y COMPARTE las
    // páginas de payload por puntero (refcount++). O(páginas + entidades).
    void snapshot_pages(ChunkPageSnapshot::ArchetypeState& out) const;
    // Repunta el storage a las páginas del snapshot (refcount++) y restaura
    // entities/ticks/num_entities + la metadata de versionado (#4).
    void restore_pages(const ChunkPageSnapshot::ArchetypeState& in);

    // ── Hot-Reload Component Schemas (#30) ──
    // Cambia el layout (stride) de un componente IN-PLACE: reasigna todas
    // las páginas del componente con el nuevo tamaño, mapeando cada campo
    // viejo a su nueva posición (vía `field_map`, pares old_byte_offset →
    // new_byte_offset con longitud) y rellenando los bytes no mapeados con
    // `default_fill` (para campos nuevos). Atómico entre ticks: no destruye
    // el mundo ni mueve entidades. Devuelve las filas migradas.

    // Descriptor de un campo migrado: viejo offset (fuente), nuevo offset
    // (destino) y longitud en bytes.
    struct FieldMap {
        size_t old_offset;
        size_t new_offset;
        size_t length;
    };

    size_t migrate_component_layout(ComponentID comp_id, size_t new_size,
                                    const FieldMap* field_map, size_t num_fields,
                                    uint8_t default_fill = 0);

    // Número de referencias de la página `chunk` de `comp_id` (1 = solo el
    // archetype; >1 = compartida con snapshots). Para tests de COW.
    size_t page_share_count(ComponentID comp_id, size_t chunk) const;

    // Puntero al array de entity IDs (Row -> Entity). Estable mientras el
    // llamador sostenga el shared lock del World. nullptr si vacío.
    const Entity* get_entities_ptr() const;

    // Puntero al array de ticks de último write para `comp_id`, o nullptr si
    // el arquetipo no tiene ese componente. Estable bajo shared lock del World.
    const uint32_t* get_last_write_ticks_ptr(ComponentID comp_id) const;

    // ── Hot/Cold Archetype Splitting (#1) ──

    // Componentes del arquetipo cuyo tier (en el store) está dentro de la
    // máscara, en orden ascendente de ComponentID (canónico).
    std::vector<ComponentID> components_in_tier(uint8_t tier_mask) const;

    // Prefetch/warm de las páginas de los componentes dados: toca la primera
    // línea de caché de cada página (hint + touch). Devuelve páginas tocadas.
    size_t prefetch_components(const std::vector<ComponentID>& comps);

    // Páginas asignadas del array de `comp_id` (0 si el arquetipo no lo tiene).
    size_t page_count(ComponentID comp_id) const;

    // Accesos (get+set) registrados a `comp_id` con profiling activo (#1).
    uint64_t access_count(ComponentID comp_id) const;

    // Decae los contadores (proporcional, sin tocar cero) — heurística #1.
    void decay_access_counts();

    // Invoca f(entity, row, last_write_tick) por cada entidad modificada después de `since_tick`.
    // Cascada "coarse → fine": salta chunks completos en O(1) (un TickRing por
    // chunk, #4) y solo escanea ticks por fila dentro de chunks sucios.
    template <typename F>
    void for_each_changed_entity(ComponentID comp_id, uint64_t since_tick, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = last_write_ticks_.find(comp_id);
        if (it == last_write_ticks_.end()) return;

        const std::vector<uint32_t>& ticks = it->second;
        auto chunks_it = dirty_chunks_.find(comp_id);
        if (chunks_it == dirty_chunks_.end()) {
            // Sin tracker (p.ej. arquetipos antiguos): escaneo directo.
            for (size_t row = 0; row < num_entities_; ++row) {
                if (ticks[row] > since_tick) {
                    f(entities_[row], row, ticks[row]);
                }
            }
            return;
        }

        const ChunkedDirtyTracker& tracker = chunks_it->second;
        const size_t chunk_size = ChunkedDirtyTracker::CHUNK_SIZE;
        const size_t num_chunks = (num_entities_ + chunk_size - 1) / chunk_size;
        for (size_t c = 0; c < num_chunks; ++c) {
            if (!tracker.chunk_has_writes_since(c, since_tick)) continue;
            size_t begin = c * chunk_size;
            size_t end = begin + chunk_size;
            if (end > num_entities_) end = num_entities_;
            for (size_t row = begin; row < end; ++row) {
                if (ticks[row] > since_tick) {
                    f(entities_[row], row, ticks[row]);
                }
            }
        }
    }

private:
    ArchetypeSignature signature_;
    size_t num_entities_ = 0;
    
    std::vector<Entity> entities_; // Row -> Entity ID

    // (#1) Referencia al registro de tipos: tier por componente + profiling.
    const ComponentStore* store_ = nullptr;

    // (#8) COW chunk pages: el payload de cada componente se guarda en
    // páginas de PAGE_ROWS filas. Las páginas se COMPARTEN con los snapshots
    // de rollback (shared_ptr); ensure_owned() las clona antes de escribir
    // si están compartidas.
    struct ChunkPage {
        std::shared_ptr<uint8_t[]> data;
    };
    std::unordered_map<ComponentID, std::vector<ChunkPage>> pages_;
    std::unordered_map<ComponentID, size_t> component_sizes_;

    // Devuelve un puntero escribible a la página `chunk` de `comp_id`
    // (clonándola si está compartida con un snapshot — copy-on-write).
    // Expande el vector de páginas si hace falta. Sin lock: el llamador
    // debe sostener el unique_lock.
    uint8_t* ensure_owned(ComponentID comp_id, size_t chunk);

    // Temporal Component Versioning (#4)
    std::unordered_map<ComponentID, std::vector<uint32_t>> last_write_ticks_;
    std::unordered_map<ComponentID, TickRing> dirty_rings_;

    // (#4) Capa gruesa a granularidad de chunk: un TickRing por rango de
    // CHUNK_SIZE filas. Skip O(1) de chunks completos en for_each_changed.
    std::unordered_map<ComponentID, ChunkedDirtyTracker> dirty_chunks_;

    // (#1) Contadores de acceso (saturantes, solo con profiling activo).
    // Estado DERIVADO: excluido de state_hash() (#11) — no rompe lockstep.
    std::unordered_map<ComponentID, uint64_t> access_counts_;
    
    mutable std::shared_mutex rw_mutex_;
};

// Contenedor principal
class World {
public:
    World(std::shared_ptr<ComponentStore> store, 
          std::shared_ptr<HistoryManager> history = nullptr,
          std::shared_ptr<fluxdb::query::SubscriptionManager> pubsub = nullptr);
    
    Entity spawn();
    void despawn(Entity entity);
    
    // Agrega componente a una entidad (causa un cambio estructural de Arquetipo)
    void add_component(Entity entity, ComponentID comp_id, const void* data);

    // TODO: remove_component
    // void remove_component(Entity entity, ComponentID comp_id);

    // Recupera todos los arquetipos para iterar (queries)
    const std::unordered_map<unsigned long long, std::unique_ptr<Archetype>>& get_archetypes() const {
        return archetypes_;
    }
    
    // Access locking for queries
    std::shared_mutex& get_mutex() const { return world_mutex_; }
    HistoryManager* get_history() const { return history_.get(); }
    ComponentStore* get_store() const { return store_.get(); }
    
    // Retrieve individual component directly (useful for networking/serialization)
    const void* get_entity_component_data(Entity entity, ComponentID comp_id, size_t& out_size) const;
    
    // Pub/Sub linkage. Si el componente se registró con tamaño == sizeof
    // (SectorPos) el World activa el modo sector-relative (#19) y todas las
    // queries espaciales pasan por el SpatialSectorGrid.
    void set_position_component_id(ComponentID id);
    fluxdb::query::SubscriptionManager* get_pubsub() const { return pubsub_.get(); }
    fluxdb::spatial::SpatialIndex* get_spatial_index() const { return spatial_index_.get(); }

    // ── Infinite World Origin Rebasing (#19) ──

    // Posición sector-relative de la entidad si el componente de posición
    // se registró con tamaño == sizeof(SectorPos) (modo sector activo) y la
    // entidad la tiene; nullptr en otro caso.
    const SectorPos* sector_position(Entity entity) const;

    // ¿El World opera con posiciones sector-relative (SectorPos)? Se detecta
    // al registrar el componente de posición (set_position_component_id).
    bool sector_positions() const { return sector_positions_; }

    // Grid espacial keyed por el MISMO grid de sectores (#19). Comparte
    // Entity con el índice de mundo legacy (que solo se usa en modo float).
    fluxdb::ecs::SpatialSectorGrid* get_sector_grid() const { return sector_grid_.get(); }

    // ── Time-Travel Collision Queries (#13) ──

    // Raycast contra el estado del mundo en `tick` pasado (o el actual con
    // tick == UINT64_MAX). Reconstruye posiciones históricas del
    // HistoryManager SIN mutar el World (a diferencia de rollback_to).
    // Devuelve la entidad más cercana intersectada (ray → esfera de radio
    // `radius` alrededor de la posición de cada entidad).
    bool raycast_historical(uint64_t tick, const fluxdb::physics::Ray& ray,
                            float radius, float max_distance,
                            fluxdb::physics::RaycastHit& out) const;

    // Raycast contra el estado ACTUAL (sin reconstrucción histórica).
    bool raycast(const fluxdb::physics::Ray& ray, float radius, float max_distance,
                 fluxdb::physics::RaycastHit& out) const;

    // Query de volumen contra el estado en `tick` pasado. Devuelve las
    // entidades cuya posición histórica cae dentro del volumen.
    void query_volume_historical(uint64_t tick, const fluxdb::physics::VolumeQuery& vol,
                                 std::vector<uint32_t>& out) const;

    // ── Interest-Managed Spatial Pub/Sub (#9) ──

    // Componentes conductores de la relevancia de red automática:
    //  - InterestVolume en una entidad → crea/actualiza su volumen de interés
    //    (AOI) en el SubscriptionManager; el volumen SIGUE al owner cuando
    //    la entidad se mueve.
    //  - Replicated en una entidad → es candidata para suscripciones
    //    replicated_only (qué replicarle a cada suscriptor).
    void set_interest_volume_component_id(ComponentID id) { interest_vol_id_ = id; }
    void set_replicated_component_id(ComponentID id) { replicated_id_ = id; }

    // Sub del volumen de interés de la entidad (0 si no tiene).
    uint32_t interest_subscription(Entity e) const;

    // (#9) Un paso de network tick: re-evalúa volúmenes dirty (móviles) y
    // entrega los batches enter/leave de cada suscriptor.
    void flush_interest_events();

    // ── Bandwidth-Aware Component LOD (#10) ──

    // Declara los tiers de replicación LOD de un componente (1..3 reglas
    // ascendentes por max_distance; cada una con frecuencia y cuantización).
    void set_component_lod(ComponentID comp_id, const std::vector<lod::LODRule>& rules);

    const lod::LodManager& lod() const { return *lod_manager_; }
    lod::LodManager& lod() { return *lod_manager_; }

    // (#10) Delta de un suscriptor: relevancia de #9 + LOD por distancia
    // (tier, frecuencia y cuantización por (entity, subscriber)). Emite
    // spawns de entidades relevantes y despawns; los componentes con tier
    // NONE no se incluyen. Alimenta el sink de red del delta engine #7.
    // (#1) Solo los tiers hot/warm se serializan por defecto: los
    // componentes COLD se saltan aunque tengan reglas LOD (los replicados
    // hot/warm se caminan primero — snapshots de red rápidos). Con
    // `include_cold_tiers = true` se incluyen (comportamiento previo).
    delta::DeltaSet build_subscriber_delta(uint32_t sub_id, uint64_t since_tick,
                                           bool include_cold_tiers = false) const;

    // ── Hot/Cold Archetype Splitting (#1) ──

    // Activa los contadores de acceso (get+set) en todos los arquetipos.
    // Overhead cero mientras esté apagado. El flag vive en el ComponentStore
    // (compartido): los arquetipos ya creados y los futuros lo respetan.
    void enable_access_profiling(bool on) { store_->set_access_profiling(on); }

    // Estadística por componente: tier, accesos (suma de arquetipos),
    // páginas y bytes totales en el World. Orden canónico por ComponentID.
    struct TierStats {
        ComponentID comp_id;
        ComponentTier tier;
        uint64_t accesses;
        size_t pages;
        size_t bytes;
    };
    std::vector<TierStats> tier_stats() const;

    // Heurística de runtime: reclasifica los componentes del store según sus
    // contadores (decay exponencial por tick vía advance_tick). Con
    // `accesos >= hot_threshold` → HOT; `<= cold_threshold` → COLD; resto WARM.
    // Devuelve cuántos componentes cambiaron de tier. No mueve datos: el
    // tier es metadato consultado en iteración/serialización.
    size_t reclassify_components(uint64_t hot_threshold = 64, uint64_t cold_threshold = 8);

    // Prefetch/warm de los tiers pedidos (máscara de TIER_MASK_*): toca la
    // primera línea de caché de cada página de los componentes de esos tiers
    // en todos los arquetipos (orden canónico). Devuelve páginas tocadas.
    size_t prefetch_tiers(uint8_t tier_mask);

    // ── Temporal Component Versioning (#4) ──

    // Avanza el reloj de simulación (VersionTracker + HistoryManager en sync).
    void advance_tick();

    // Tick actual. Con HistoryManager adjunto, usa el tick de este para
    // garantizar que versioning y time-travel sellen los mismos valores.
    uint64_t current_tick() const { return history_ ? history_->get_current_tick() : version_.current_tick(); }

    VersionTracker* get_version() { return &version_; }

    // Query primitiva: `query.changed<T>(since_tick)`.
    // Filtra a nivel arquetipo en O(1) (coarse ring) y luego por entidad.
    // Iteración en ORDEN CANÓNICO (#11): arquetipos por firma ascendente.
    template <typename F>
    void for_each_changed(ComponentID comp_id, uint64_t since_tick, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(world_mutex_);
        for (Archetype* arch : canonical_archetypes()) {
            if (!arch->has_component(comp_id)) continue;
            if (!arch->has_any_write_since(comp_id, since_tick)) continue;
            arch->for_each_changed_entity(comp_id, since_tick, std::forward<F>(f));
        }
    }

    // ¿La entidad modificó este componente después de `since_tick`?
    bool entity_changed_since(Entity entity, ComponentID comp_id, uint64_t since_tick) const;

    // Tick del último write de la entidad en este componente (0 = nunca).
    uint32_t entity_last_write_tick(Entity entity, ComponentID comp_id) const;

    // ── Compiled Query Plans (#5) ──

    // Compila (y cachea) una query por firma de componentes.
    // Queries con la misma firma devuelven el mismo handle (dedupe).
    // El plan se actualiza solo cuando un arquetipo NUEVO matchea (invalidación selectiva).
    QueryHandle create_query(std::vector<ComponentID> required);

    // Convenience: world.create_query({pos_id, hp_id})
    QueryHandle create_query(std::initializer_list<ComponentID> required) {
        return create_query(std::vector<ComponentID>(required));
    }

    const QueryPlan* get_query_plan(QueryHandle handle) const;

    // Remueve un arquetipo del World (debe estar vacío). Invalida
    // selectivamente los planes de query que lo matcheaban (#5) y limpia el
    // índice componente→arquetipos. Devuelve false si no existe o no está vacío.
    bool remove_archetype(ArchetypeSignature sig);

    // ── Hot-Reload Component Schemas Without World Reset (#30) ──

    // Migra en caliente el layout (tamaño) de un componente en TODOS los
    // arquetipos que lo contienen, sin destruir el mundo ni mover entidades.
    // `field_map` reasigna viejos offsets a nuevos; los bytes no mapeados se
    // rellenan con `default_fill` (campos nuevos). Actualiza el tamaño en el
    // store para que writes futuros usen el nuevo stride. Devuelve filas migradas.
    size_t hot_reload_component(ComponentID comp_id, size_t new_size,
                                const Archetype::FieldMap* field_map, size_t num_fields,
                                uint8_t default_fill = 0);

    // ── Unified Delta Engine (#7) ──

    // Spawn determinista con ID explícito (replay/netcode). Idempotente:
    // si el ID ya existe, devuelve la entidad existente.
    Entity spawn_with_id(Entity id);

    // Salto determinista del reloj (VersionTracker + HistoryManager en sync).
    void advance_to(uint64_t tick);

    // Registro de codecs por componente (RAW, QUANTIZED_FLOAT, RLE, BITPACK).
    delta::CodecRegistry& codec_registry() const;

    // Convenience: configura el codec de un componente.
    void set_codec(ComponentID comp_id, delta::CodecID codec);

    // Convenience (#7): configura el codec de un componente desde el trait
    // compile-time DeltaCodec<T> (especializado por tipo C++).
    template <typename T>
    void set_codec(ComponentID comp_id) {
        set_codec(comp_id, delta::DeltaCodec<T>::id());
    }

    // Evento estructural sellado por tick (alimenta los sinks de #7).
    struct StructuralEvent {
        uint64_t tick;
        Entity entity;
        bool spawned; // true = spawn, false = despawn
    };

    // Log inmutable de spawn/despawn desde la creación del World.
    // El llamador no debe mutar el World concurrentemente mientras lee.
    const std::vector<StructuralEvent>& structural_events() const;

    // Poda eventos estructurales sellados en o antes de `before_tick`.
    void prune_structural_events(uint64_t before_tick);

    // Saves (#18): base snapshot + cadena de deltas (formato replay unificado).
    bool save_incremental(const std::string& path);
    bool save_incremental(const std::string& path, const std::string& from_existing);
    bool load_from_replay(const std::string& path);

    // Compactación de saves (#7): pliega la cadena de deltas del archivo en
    // su snapshot base (estado final en un único snapshot, 0 ticks).
    bool compact_save(const std::string& path);

    // ── Native Entity Relationship Graphs (#6) ──

    // (src, kind, dst) con payload opcional. Sella el versionado de #4
    // (fino por (src,kind) + grueso por kind) en el tick actual.
    void add_relation(Entity src, RelationKind kind, Entity dst, RelationPayload payload = {});

    bool remove_relation(Entity src, RelationKind kind, Entity dst);

    bool has_relation(Entity src, RelationKind kind, Entity dst) const;

    bool get_relation(Entity src, RelationKind kind, Entity dst, RelationPayload& out) const;

    size_t outgoing_degree(Entity src, RelationKind kind) const;
    size_t incoming_degree(Entity target, RelationKind kind) const;

    // Forward: f(dst, payload) — hijos, dependencias...
    template <typename F>
    void for_each_outgoing_relation(Entity src, RelationKind kind, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(world_mutex_);
        relations_.for_each_outgoing(src, kind, std::forward<F>(f));
    }

    // Backward O(degree): f(src, payload) — "quién apunta a X".
    template <typename F>
    void for_each_incoming_relation(Entity target, RelationKind kind, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(world_mutex_);
        relations_.for_each_incoming(target, kind, std::forward<F>(f));
    }

    // Coarse O(1): ¿cambió alguna relación de `kind` después de `since_tick`?
    bool relation_kind_changed_since(RelationKind kind, uint64_t since_tick) const;

    // Fino: tick del último cambio de relaciones (src, kind). 0 = nunca.
    uint32_t relation_last_write_tick(RelationKind kind, Entity src) const;

    // Acceso directo al grafo (para serialización de #7 y tooling).
    RelationGraph& relations() { return relations_; }
    const RelationGraph& relations() const { return relations_; }

    // ── First-Class Rollback Netcode (#8) ──

    // Reset total: arquetipos, entidades, eventos estructurales, planes de
    // query (se recompilan bajo demanda) e índice espacial. `next_entity_`
    // no se resetea (spawn_with_id restaura IDs explícitos).
    void clear_all();

    // Adjunta un SnapshotRingBuffer para rollback/resimulación.
    void attach_rollback(rollback::SnapshotRingBuffer* ring) { rollback_ring_ = ring; }

    // Restaura el estado exacto en `tick` (base snapshot + deltas re-aplicados
    // hacia adelante). Requiere un ring adjunto que cubra `tick`.
    bool rollback_to(uint64_t tick);

    // Rollback a `from_tick` + re-aplica los deltas grabados hasta `to_tick`
    // (la versión de "resimular hacia adelante" con inputs grabados).
    bool resimulate(uint64_t from_tick, uint64_t to_tick);

    // (#8) Resimulación con inputs externos (GGPO-style): rollback a
    // `from_tick` y replay hacia adelante. Por cada tick, primero se aplica
    // el delta grabado (si existe) y LUEGO los inputs de ese tick — los
    // inputs CORRIGEN el estado grabado y ganan. Inputs fuera del rango
    // (tick <= from o > to) se ignoran.
    bool resimulate(uint64_t from_tick, uint64_t to_tick, const std::vector<rollback::ExternalInput>& inputs);

    // (#8) COW structural snapshot: captura el estado completo como páginas
    // de chunks compartidas (copy-on-write) + metadatos estructurales.
    void capture_chunk_pages(ChunkPageSnapshot& out) const;

    // (#8) Restaura el estado de un COW snapshot: recrea los arquetipos y
    // repunta sus páginas a las versiones históricas (sin copiar payloads).
    // Los eventos estructurales se re-sellan al tick del snapshot.
    void restore_chunk_pages(const ChunkPageSnapshot& in);

    // ── Deterministic Lockstep Mode (#11) ──

    // Sella el world en modo determinista: la iteración de arquetipos y
    // consultas queda garantizada en orden canónico (firma ascendente).
    void enable_determinism_lock();
    bool determinism_locked() const { return determinism_locked_; }

    // Hash FNV-1a 64 del estado completo en orden canónico: arquetipos por
    // firma ascendente, entidades por fila, componentes y ticks de versión,
    // relaciones ordenadas, eventos estructurales y tick actual. El mismo
    // estado → el mismo hash en cualquier plataforma (desync detection).
    uint64_t state_hash() const;

    // (#11) Aplica inputs externos del tick en curso (mismo formato que la
    // resimulación GGPO de #8). Cada input sella su write en el tick actual.
    void apply_inputs(const std::vector<rollback::ExternalInput>& inputs);

    // (#11) Iteración de arquetipos en ORDEN CANÓNICO (firma ascendente) —
    // el reemplazo determinista de get_archetypes() (unordered_map) para
    // cualquier consumo que afecte el estado de la simulación.
    template <typename F>
    void for_each_archetype_sorted(F&& f) const {
        std::shared_lock<std::shared_mutex> lock(world_mutex_);
        for (Archetype* arch : canonical_archetypes()) {
            f(arch);
        }
    }

    ~World();

private:
    Archetype* get_or_create_archetype(ArchetypeSignature sig);
    void move_entity(Entity entity, Archetype* old_arch, size_t old_row, Archetype* new_arch);

    // (#11) Arquetipos ordenados por firma ascendente (orden canónico).
    // El llamador debe sostener el lock (shared o unique) del World.
    std::vector<Archetype*> canonical_archetypes() const;

    // (#8) Aplica un input externo al world en su tick (sella versionado).
    void apply_external_input(const rollback::ExternalInput& in);

    // (#9) Crea/actualiza la suscripción de volumen de interés de la entidad.
    void upsert_interest_volume(Entity e, const fluxdb::query::InterestVolume& vol);

    Entity next_entity_ = 0;
    std::shared_ptr<ComponentStore> store_;
    std::shared_ptr<HistoryManager> history_;
    std::shared_ptr<fluxdb::query::SubscriptionManager> pubsub_;
    std::unique_ptr<fluxdb::spatial::SpatialIndex> spatial_index_;
    // (#19) Grid espacial sector-relative (activo cuando el componente de
    // posición se registró como SectorPos). Estado DERIVADO (excluido del
    // state_hash #11).
    std::unique_ptr<fluxdb::ecs::SpatialSectorGrid> sector_grid_;
    bool sector_positions_ = false;
    VersionTracker version_;
    ComponentID pos_id_ = 255;

    // Interest-Managed Spatial Pub/Sub (#9)
    ComponentID interest_vol_id_ = 255;
    ComponentID replicated_id_ = 255;
    std::unordered_set<Entity> replicated_entities_;
    std::unordered_map<Entity, uint32_t> entity_interest_subs_;

    // Bandwidth-Aware Component LOD (#10)
    std::unique_ptr<lod::LodManager> lod_manager_;

    // Unified Delta Engine (#7)
    std::unique_ptr<delta::CodecRegistry> codec_registry_;
    std::vector<StructuralEvent> structural_events_;

    // Native Entity Relationship Graphs (#6)
    RelationGraph relations_;

    // First-Class Rollback Netcode (#8)
    rollback::SnapshotRingBuffer* rollback_ring_ = nullptr;

    // Deterministic Lockstep Mode (#11)
    bool determinism_locked_ = false;
    
    struct EntityLocation {
        Archetype* archetype;
        size_t row;
    };
    std::unordered_map<Entity, EntityLocation> entity_locations_;
    
    // Archivo central de Arquetipos (Firma literal UL -> Arquetipo)
    std::unordered_map<unsigned long long, std::unique_ptr<Archetype>> archetypes_;
    mutable std::shared_mutex world_mutex_;

    // Compiled Query Plans (#5)
    std::vector<std::unique_ptr<QueryPlan>> plans_;
    std::unordered_map<unsigned long long, QueryHandle> query_by_signature_;

    // Índice de firma de componentes: ComponentID -> arquetipos que lo contienen.
    // Alimenta la invalidación selectiva de planes y consumidores futuros (#7/#9).
    std::unordered_map<ComponentID, std::vector<Archetype*>> component_archetype_index_;
};

} // namespace ecs
} // namespace fluxdb
