#pragma once

#include <cstdint>
#include <vector>
#include <set>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <cstring>

namespace fluxdb {
namespace ecs {

// Represents a single localized change in time for an entity's component
struct HistoryRecord {
    uint64_t tick;
    uint32_t entity;
    uint8_t component_id;
    
    // We store max 256 bytes per component per tick for this demo
    // In production, this would be a dynamic pointer to a memory pool
    uint8_t old_data[256];
    size_t data_size;
};

// A circular buffer or log that tracks every state change
class HistoryManager {
public:
    HistoryManager(uint64_t max_ticks_to_keep = 600) // 10 seconds at 60Hz
        : max_ticks_to_keep_(max_ticks_to_keep), current_tick_(0) {}

    // Called every network / core game loop frame
    void advance_tick();
    uint64_t get_current_tick() const { return current_tick_; }

    // Salto determinista del reloj (replay de #7): fija el tick sin loggear.
    void advance_to(uint64_t t);

    // Save the old state of a component before it gets overwritten
    void record_change(uint32_t e, uint8_t comp_id, const void* old_data, size_t size);

    // Reconstruct the exact bytes of a component at a given past tick
    // Returns true if data was found, false if we must fallback to the CURRENT live ECS value
    bool get_historical_state(uint64_t target_tick, uint32_t e, uint8_t comp_id, void* out_data, size_t size) const;
    
    // Returns all unique (Entity, ComponentID) pairs modified strictly AFTER `from_tick`
    void get_modifications_since(uint64_t from_tick, std::set<std::pair<uint32_t, uint8_t>>& out_dirty) const;

private:
    uint64_t max_ticks_to_keep_;
    uint64_t current_tick_;

    // Thread safety for history reads/writes
    mutable std::shared_mutex mutex_;

    // Tick -> List of all changes that happened IN THAT TICK
    // Storing it tick->changes makes cleanup of old ticks very fast
    std::unordered_map<uint64_t, std::vector<HistoryRecord>> history_log_;
};

} // namespace ecs
} // namespace fluxdb
