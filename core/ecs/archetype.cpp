// Agrupación por tipo → Agrupación por tipo (como Bevy/Unity DOTS), etc
// Debemos crear una estructura de datos que permita almacenar los componentes de manera contigua en memoria.
// Esto permite un mejor rendimiento debido a la localidad de caché.

#include "../headers/ecs.h"
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace fluxdb {
namespace ecs {

Archetype::Archetype(ArchetypeSignature sig, const ComponentStore& store)
    : signature_(sig) {
    // Para cada bit de la firma, registrar su tamaño y preparar el vector de bytes
    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (sig.test(i)) {
            ComponentID id = static_cast<ComponentID>(i);
            size_t size = store.get_info(id).size;
            component_sizes_[id] = size;
            component_data_[id]  = std::vector<uint8_t>();
        }
    }
}

size_t Archetype::add_entity(Entity entity) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    size_t row = num_entities_;
    entities_.push_back(entity);

    // Expandir cada array de componentes por +1 slot
    for (auto& [id, vec] : component_data_) {
        size_t size = component_sizes_[id];
        vec.resize(vec.size() + size, 0); // rellenamos de ceros
    }

    num_entities_++;
    return row;
}

Entity Archetype::remove_entity(size_t row) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    
    if (row >= num_entities_) return UINT32_MAX;

    size_t last_row = num_entities_ - 1;
    Entity moved_entity = UINT32_MAX;

    // Swap and pop
    if (row != last_row) {
        // Mover Entity ID
        entities_[row] = entities_[last_row];
        moved_entity = entities_[row];

        // Mover Data
        for (auto& [id, vec] : component_data_) {
            size_t size = component_sizes_[id];
            
            uint8_t* dst = vec.data() + (row * size);
            uint8_t* src = vec.data() + (last_row * size);
            std::memcpy(dst, src, size);
        }
    }

    entities_.pop_back();

    for (auto& [id, vec] : component_data_) {
        size_t size = component_sizes_[id];
        vec.resize(vec.size() - size);
    }

    num_entities_--;
    return moved_entity;
}

uint8_t* Archetype::get_component_array(ComponentID comp_id) {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = component_data_.find(comp_id);
    if (it != component_data_.end()) {
        return it->second.data();
    }
    return nullptr;
}

void Archetype::set_component_data(size_t row, ComponentID comp_id, const void* data, Entity entity, HistoryManager* history) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = component_data_.find(comp_id);
    if (it != component_data_.end()) {
        size_t size = component_sizes_[comp_id];
        uint8_t* dst = it->second.data() + (row * size);
        
        // Push current memory state to History (Time-Travel) before overwriting
        if (history && entity != UINT32_MAX) {
            history->record_change(entity, comp_id, dst, size);
        }

        std::memcpy(dst, data, size);
    }
}

void* Archetype::get_component_data(size_t row, ComponentID comp_id) {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = component_data_.find(comp_id);
    if (it != component_data_.end()) {
        size_t size = component_sizes_[comp_id];
        return it->second.data() + (row * size);
    }
    return nullptr;
}

} // namespace ecs
} // namespace fluxdb
