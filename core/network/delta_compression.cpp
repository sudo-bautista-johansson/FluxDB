#include "../headers/network.h"
#include <set>
#include <cstring>
#include <iostream>

namespace veldradb {
namespace network {

void DeltaCompression::generate_delta_payload(uint64_t last_ack_tick, std::vector<uint8_t>& out_buffer) const {
    out_buffer.clear();
    
    if (!world_ || !world_->get_history()) {
        // No history tracking enabled, delta compression impossible
        return;
    }
    
    auto* history = world_->get_history();
    uint64_t current_tick = history->get_current_tick();
    
    // 1. Gather all unique (Entity, ComponentID) pairs modified between last_ack_tick and current_tick.
    // By using a set, we avoid sending the same component twice if it mutated multiple times.
    std::set<std::pair<uint32_t, uint8_t>> dirty_components;
    
    // Wait, history_log_ is private in HistoryManager. 
    // We need a way to ask HistoryManager for modifications.
    history->get_modifications_since(last_ack_tick, dirty_components);
    
    // 2. Build the payload
    // Header
    uint32_t num_records = static_cast<uint32_t>(dirty_components.size());
    size_t offset = 0;
    out_buffer.resize(sizeof(uint32_t));
    std::memcpy(out_buffer.data(), &num_records, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    const auto& archetypes = world_->get_archetypes();
    std::shared_lock<std::shared_mutex> world_lock(world_->get_mutex());
    
    for (const auto& pair : dirty_components) {
        uint32_t ent = pair.first;
        uint8_t comp_id = pair.second;
        
        // Find entity in ECS
        // (Since entity_locations_ is private, we'll need to add a world_->get_entity_data(ent, comp_id, &size) method).
        size_t size_out = 0;
        const void* data = world_->get_entity_component_data(ent, comp_id, size_out);
        
        if (data) {
            uint8_t u8_size = static_cast<uint8_t>(size_out);
            
            // Realloc buffer
            out_buffer.resize(out_buffer.size() + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + u8_size);
            
            std::memcpy(out_buffer.data() + offset, &ent, sizeof(uint32_t)); offset += sizeof(uint32_t);
            std::memcpy(out_buffer.data() + offset, &comp_id, sizeof(uint8_t)); offset += sizeof(uint8_t);
            std::memcpy(out_buffer.data() + offset, &u8_size, sizeof(uint8_t)); offset += sizeof(uint8_t);
            std::memcpy(out_buffer.data() + offset, data, u8_size); offset += u8_size;
        }
    }
}

} // namespace network
} // namespace veldradb
