#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <shared_mutex>
#include <cmath>

namespace fluxdb {
namespace query {

// Represents a native subscription to an area of the world
struct QuerySubscription {
    uint32_t sub_id;
    std::string target_prefab; // empty means any entity
    float center_x, center_y, center_z;
    float radius;
    
    // Callback: gets Called when an entity enters (true) or leaves (false) the radius
    std::function<void(uint32_t entity_id, bool entered)> callback;
};

class SubscriptionManager {
public:
    SubscriptionManager() = default;

    // Register a new spatial listener
    uint32_t subscribe_spatial(const std::string& prefab, float x, float y, float z, float r, 
                               std::function<void(uint32_t, bool)> cb);
                               
    void unsubscribe(uint32_t sub_id);
    
    // Called by the engine whenever an entity moves.
    // It compares the distance of the old and new positions against active subscriptions.
    void notify_entity_moved(uint32_t e, const std::string& prefab, 
                             float old_x, float old_y, float old_z,
                             float new_x, float new_y, float new_z);

private:
    uint32_t next_id_ = 1;
    mutable std::shared_mutex mutex_;
    std::vector<QuerySubscription> subscriptions_;

    bool is_inside(float x, float y, float z, const QuerySubscription& sub) const;
};

} // namespace query
} // namespace fluxdb
