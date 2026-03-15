#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <bitset>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include <shared_mutex>
#include "history.h"
#include "pubsub.h"
#include "spatial_index.h"

namespace fluxdb {
namespace ecs {

using Entity = uint32_t;
using ComponentID = uint8_t;
constexpr size_t MAX_COMPONENTS = 64;
using ArchetypeSignature = std::bitset<MAX_COMPONENTS>;

// Definición de información sobre un componente
struct ComponentInfo {
    ComponentID id;
    std::string name;
    size_t size;
};

// Almacén y registro central de Tipos de Componentes
class ComponentStore {
public:
    ComponentStore() = default;
    
    ComponentID register_component(const std::string& name, size_t size);
    ComponentID get_id(const std::string& name) const;
    const ComponentInfo& get_info(ComponentID id) const;
    size_t count() const { return components_.size(); }

private:
    std::vector<ComponentInfo> components_;
    std::unordered_map<std::string, ComponentID> name_to_id_;
};

// Patrón Data-Oriented Design (SoA).
// Almacena datos contiguos de entidades que comparten la misma firma.
class Archetype {
public:
    Archetype(ArchetypeSignature sig, const ComponentStore& store);
    
    ArchetypeSignature get_signature() const { return signature_; }
    size_t get_entity_count() const { return num_entities_; }
    
    // Añade una entidad vacía al final y devuelve su "fila" (índice)
    size_t add_entity(Entity entity);
    
    // Elimina la entidad en la 'fila' (swap con el último y pop)
    // Devuelve la Entity que fue movida (para actualizar su row) o UINT32_MAX si no movió a nadie
    Entity remove_entity(size_t row);
    
    // Array contiguo (byte buffer) para el ComponentID solicitado
    uint8_t* get_component_array(ComponentID comp_id);
    
    // Escribe datos RAW para el componente en la fila especificada
    void set_component_data(size_t row, ComponentID comp_id, const void* data, Entity entity, HistoryManager* history = nullptr);

    // Lee datos RAW
    void* get_component_data(size_t row, ComponentID comp_id);

private:
    ArchetypeSignature signature_;
    size_t num_entities_ = 0;
    
    std::vector<Entity> entities_; // Row -> Entity ID

    // SoA: Un arreglo de bytes flat para cada ComponentID
    std::unordered_map<ComponentID, std::vector<uint8_t>> component_data_;
    std::unordered_map<ComponentID, size_t> component_sizes_;
    
    mutable std::shared_mutex rw_mutex_;
};

// Contenedor principal
class World {
public:
    World(std::shared_ptr<ComponentStore> store, 
          std::shared_ptr<HistoryManager> history = nullptr,
          std::shared_ptr<veldradb::query::SubscriptionManager> pubsub = nullptr);
    
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
    
    // Pub/Sub linkage
    void set_position_component_id(ComponentID id) { pos_id_ = id; }
    veldradb::query::SubscriptionManager* get_pubsub() const { return pubsub_.get(); }
    veldradb::spatial::SpatialIndex* get_spatial_index() const { return spatial_index_.get(); }

private:
    Archetype* get_or_create_archetype(ArchetypeSignature sig);
    void move_entity(Entity entity, Archetype* old_arch, size_t old_row, Archetype* new_arch);

    Entity next_entity_ = 0;
    std::shared_ptr<ComponentStore> store_;
    std::shared_ptr<HistoryManager> history_;
    std::shared_ptr<veldradb::query::SubscriptionManager> pubsub_;
    std::unique_ptr<veldradb::spatial::SpatialIndex> spatial_index_;
    ComponentID pos_id_ = 255;
    
    struct EntityLocation {
        Archetype* archetype;
        size_t row;
    };
    std::unordered_map<Entity, EntityLocation> entity_locations_;
    
    // Archivo central de Arquetipos (Firma literal UL -> Arquetipo)
    std::unordered_map<unsigned long long, std::unique_ptr<Archetype>> archetypes_;
    mutable std::shared_mutex world_mutex_;
};

} // namespace ecs
} // namespace fluxdb
