#include "../headers/history.h"
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <cstring>

namespace fluxdb {
namespace ecs {

void HistoryManager::advance_tick() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    current_tick_++;

    // Prune very old ticks to save memory
    if (current_tick_ > max_ticks_to_keep_) {
        uint64_t prune_tick = current_tick_ - max_ticks_to_keep_;
        history_log_.erase(prune_tick);
    }
}

void HistoryManager::advance_to(uint64_t t) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    current_tick_ = t;
}

void HistoryManager::record_change(uint32_t e, uint8_t comp_id, const void* old_data, size_t size) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // We cap size to 256 bytes per component for safety in this demo
    size_t copy_size = (size > 256) ? 256 : size;
    
    HistoryRecord rec;
    rec.tick = current_tick_;
    rec.entity = e;
    rec.component_id = comp_id;
    rec.data_size = copy_size;
    
    if (old_data) {
        std::memcpy(rec.old_data, old_data, copy_size);
    } else {
        std::memset(rec.old_data, 0, 256);
    }

    history_log_[current_tick_].push_back(rec);
}

bool HistoryManager::get_historical_state(uint64_t target_tick, uint32_t e, uint8_t comp_id, void* out_data, size_t size) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (target_tick > current_tick_) {
        return false; // Can't query the future
    }
    
    if (current_tick_ - target_tick >= max_ticks_to_keep_) {
        // Asked for a tick that has already been pruned. We shouldn't crash,
        // but it's technically an invalid query. For a game engine, you might throw or warn.
        // We will just return false, falling back to current state.
        return false;
    }

    // Logic: The value at target_tick X is equal to the state BEFORE the first change
    // that happened strictly AFTER target_tick X.
    // So we iterate backwards from `current_tick_` down to `target_tick + 1`.
    // The very FIRST record we find going backwards in time for that Entity+Component 
    // is the exact data the player had at target_tick.
    
    for (uint64_t t = target_tick + 1; t <= current_tick_; ++t) {
        auto it = history_log_.find(t);
        if (it != history_log_.end()) {
            // Find if this entity changed in this tick
            for (auto rec_it = it->second.begin(); rec_it != it->second.end(); ++rec_it) {
                if (rec_it->entity == e && rec_it->component_id == comp_id) {
                    // FOUND! This is the state right before the first change that happened
                    // AFTER target_tick. That implies this was the state at strictly `target_tick`.
                    size_t copy_size = (size > rec_it->data_size) ? rec_it->data_size : size;
                    std::memcpy(out_data, rec_it->old_data, copy_size);
                    return true;
                }
            }
        }
    }

    // If we iterated all the way down and found NO changes, it means the entity's 
    // current live state in the ECS has never changed since `target_tick`.
    // We return false to signal "Fallback to standard ECS Archetype read".
    return false;
}

void HistoryManager::get_modifications_since(uint64_t from_tick, std::set<std::pair<uint32_t, uint8_t>>& out_dirty) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (uint64_t t = from_tick + 1; t <= current_tick_; ++t) {
        auto it = history_log_.find(t);
        if (it != history_log_.end()) {
            for (const auto& rec : it->second) {
                out_dirty.insert({rec.entity, rec.component_id});
            }
        }
    }
}

} // namespace ecs
} // namespace fluxdb
