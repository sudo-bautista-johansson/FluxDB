#include "../headers/pubsub.h"
#include <algorithm>

namespace veldradb {
namespace query {

uint32_t SubscriptionManager::subscribe_spatial(const std::string& prefab, float x, float y, float z, float r, 
                                                std::function<void(uint32_t, bool)> cb) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    uint32_t id = next_id_++;
    
    QuerySubscription sub;
    sub.sub_id = id;
    sub.target_prefab = prefab;
    sub.center_x = x;
    sub.center_y = y;
    sub.center_z = z;
    sub.radius = r;
    sub.callback = std::move(cb);
    
    subscriptions_.push_back(std::move(sub));
    return id;
}

void SubscriptionManager::unsubscribe(uint32_t sub_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    subscriptions_.erase(
        std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                       [sub_id](const QuerySubscription& s) { return s.sub_id == sub_id; }),
        subscriptions_.end()
    );
}

bool SubscriptionManager::is_inside(float x, float y, float z, const QuerySubscription& sub) const {
    float dx = x - sub.center_x;
    float dy = y - sub.center_y;
    float dz = z - sub.center_z;
    float dist_sq = (dx*dx) + (dy*dy) + (dz*dz);
    return dist_sq <= (sub.radius * sub.radius);
}

void SubscriptionManager::notify_entity_moved(uint32_t e, const std::string& prefab, 
                                              float old_x, float old_y, float old_z,
                                              float new_x, float new_y, float new_z) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    for (const auto& sub : subscriptions_) {
        if (!sub.target_prefab.empty() && sub.target_prefab != prefab) {
            continue; // Not the type of entity we are tracking
        }
        
        bool was_inside = is_inside(old_x, old_y, old_z, sub);
        bool is_now_inside = is_inside(new_x, new_y, new_z, sub);
        
        if (was_inside && !is_now_inside) {
            // Trigger Exited
            sub.callback(e, false);
        } else if (!was_inside && is_now_inside) {
            // Trigger Entered
            sub.callback(e, true);
        }
    }
}

} // namespace query
} // namespace veldradb
