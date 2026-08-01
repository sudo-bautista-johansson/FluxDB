#include "../headers/pubsub.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace fluxdb {
namespace query {

uint32_t SubscriptionManager::do_subscribe(const std::string& prefab, const InterestVolume& vol,
                                           bool replicated_only, std::function<void(uint32_t, bool)> cb) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    uint32_t id = next_id_++;

    QuerySubscription sub;
    sub.sub_id = id;
    sub.target_prefab = prefab;
    sub.volume = vol;
    sub.replicated_only = replicated_only;
    sub.callback = std::move(cb);

    // El backfill (qué hay ya dentro del volumen) se resuelve en el primer
    // refresh_volumes(): consultar el índice espacial aquí bloquearía al
    // World (que nos llama sosteniendo su lock).
    sub.volume_dirty = true;

    subscriptions_.push_back(std::move(sub));
    return id;
}

uint32_t SubscriptionManager::subscribe_spatial(const std::string& prefab, float x, float y, float z, float r,
                                                std::function<void(uint32_t, bool)> cb) {
    InterestVolume vol;
    vol.shape = VolumeShape::SPHERE;
    vol.cx = x; vol.cy = y; vol.cz = z;
    vol.r = r;
    return do_subscribe(prefab, vol, false, std::move(cb));
}

uint32_t SubscriptionManager::subscribe_volume(const std::string& prefab, const InterestVolume& vol,
                                               std::function<void(uint32_t, bool)> cb) {
    return do_subscribe(prefab, vol, false, std::move(cb));
}

uint32_t SubscriptionManager::subscribe_replicated_volume(const std::string& prefab, const InterestVolume& vol,
                                                          std::function<void(uint32_t, bool)> cb) {
    return do_subscribe(prefab, vol, true, std::move(cb));
}

void SubscriptionManager::unsubscribe(uint32_t sub_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    subscriptions_.erase(
        std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                       [sub_id](const QuerySubscription& s) { return s.sub_id == sub_id; }),
        subscriptions_.end()
    );
}

QuerySubscription* SubscriptionManager::find_sub(uint32_t sub_id) {
    for (auto& s : subscriptions_) {
        if (s.sub_id == sub_id) return &s;
    }
    return nullptr;
}

const QuerySubscription* SubscriptionManager::find_sub(uint32_t sub_id) const {
    for (const auto& s : subscriptions_) {
        if (s.sub_id == sub_id) return &s;
    }
    return nullptr;
}

void SubscriptionManager::apply_enter_leave(QuerySubscription& sub, uint32_t e, bool now_inside) {
    bool was_inside = sub.inside.count(e) > 0;
    if (was_inside == now_inside) return; // sin cambio de relevancia
    if (now_inside) {
        sub.inside.insert(e);
    } else {
        sub.inside.erase(e);
    }
    sub.pending.push_back({e, now_inside});
}

void SubscriptionManager::update_volume(uint32_t sub_id, const InterestVolume& vol) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    QuerySubscription* sub = find_sub(sub_id);
    if (!sub) return;
    sub->volume = vol;
    sub->volume_dirty = true; // volumen móvil: refresh en el próximo tick
}

void SubscriptionManager::refresh_volumes() {
    // Recolectar los ids dirty FUERA de las consultas al índice (el query_fn
    // del World toma su lock compartido; no podemos sostener el nuestro).
    std::vector<std::pair<uint32_t, InterestVolume>> dirty;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& s : subscriptions_) {
            if (s.volume_dirty) {
                dirty.emplace_back(s.sub_id, s.volume);
                s.volume_dirty = false;
            }
        }
    }

    if (!query_fn_) return;

    for (const auto& [sub_id, vol] : dirty) {
        float bx, by, bz, br;
        vol.bounding_sphere(bx, by, bz, br);

        std::vector<SpatialCandidate> candidates;
        query_fn_(bx, by, bz, br, candidates);

        std::unique_lock<std::shared_mutex> lock(mutex_);
        QuerySubscription* sub = find_sub(sub_id);
        if (!sub) continue; // se desuscribió mientras tanto

        // Nuevo conjunto exacto (filtros + test de forma) → diff contra el
        // estado actual de relevancia.
        std::unordered_set<uint32_t> new_inside;
        for (const auto& c : candidates) {
            if (!sub->target_prefab.empty()) continue; // sin prefab en el índice
            if (sub->replicated_only && replicated_check_fn_ && !replicated_check_fn_(c.entity)) continue;
            if (vol.contains(c.x, c.y, c.z)) {
                new_inside.insert(c.entity);
            }
        }
        for (uint32_t e : sub->inside) {
            if (!new_inside.count(e)) apply_enter_leave(*sub, e, false);
        }
        for (uint32_t e : new_inside) {
            if (!sub->inside.count(e)) apply_enter_leave(*sub, e, true);
        }
    }
}

void SubscriptionManager::flush_events() {
    std::vector<std::pair<std::function<void(uint32_t, bool)>, std::vector<InterestEvent>>> batches;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& s : subscriptions_) {
            if (s.pending.empty()) continue;
            batches.emplace_back(s.callback, std::move(s.pending));
            s.pending.clear();
        }
    }
    // Callbacks FUERA del lock (pueden re-entrar al world).
    for (auto& [cb, events] : batches) {
        if (!cb) continue;
        for (const auto& ev : events) {
            cb(ev.entity, ev.entered);
        }
    }
}

const std::unordered_set<uint32_t>& SubscriptionManager::relevant_entities(uint32_t sub_id) const {
    static const std::unordered_set<uint32_t> empty;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const QuerySubscription* sub = find_sub(sub_id);
    return sub ? sub->inside : empty;
}

bool SubscriptionManager::volume_center(uint32_t sub_id, float& x, float& y, float& z) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const QuerySubscription* sub = find_sub(sub_id);
    if (!sub) return false;
    x = sub->volume.cx;
    y = sub->volume.cy;
    z = sub->volume.cz;
    return true;
}

void SubscriptionManager::replication_diff(uint32_t sub_id, std::vector<uint32_t>& out_enter,
                                           std::vector<uint32_t>& out_leave) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    QuerySubscription* sub = find_sub(sub_id);
    if (!sub) return;

    for (auto it = sub->known.begin(); it != sub->known.end();) {
        if (!sub->inside.count(*it)) {
            out_leave.push_back(*it);
            it = sub->known.erase(it);
        } else {
            ++it;
        }
    }
    for (uint32_t e : sub->inside) {
        if (!sub->known.count(e)) {
            out_enter.push_back(e);
            sub->known.insert(e);
        }
    }
}

void SubscriptionManager::notify_entity_despawned(uint32_t entity) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& sub : subscriptions_) {
        if (sub.inside.erase(entity) > 0) {
            sub.pending.push_back({entity, false});
        }
    }
}

void SubscriptionManager::notify_entity_moved(uint32_t e, const std::string& prefab,
                                              float old_x, float old_y, float old_z,
                                              float new_x, float new_y, float new_z) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    for (auto& sub : subscriptions_) {
        if (!sub.target_prefab.empty() && sub.target_prefab != prefab) {
            continue;
        }
        if (sub.replicated_only && replicated_check_fn_ && !replicated_check_fn_(e)) {
            continue;
        }
        bool now_inside = sub.volume.contains(new_x, new_y, new_z);
        apply_enter_leave(sub, e, now_inside);
    }
}

} // namespace query
} // namespace fluxdb
