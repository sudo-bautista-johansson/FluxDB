#include "../headers/relations.h"

namespace fluxdb {
namespace ecs {

void RelationGraph::stamp(RelationKind kind, Entity src, uint64_t tick) {
    if (tick == 0) return;
    if (kind >= MAX_RELATION_KINDS) return;
    last_write_ticks_[key(src, kind)] = static_cast<uint32_t>(tick);
    kind_rings_[kind].mark(tick);
}

void RelationGraph::add_relation(Entity src, RelationKind kind, Entity dst, RelationPayload payload, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    // Resurrección: si la arista estaba tombstoned, la tumba se descarta.
    uint64_t tkey = key(src, kind);
    auto tit = tombstones_.find(tkey);
    if (tit != tombstones_.end()) {
        auto& tl = tit->second;
        tl.erase(std::remove_if(tl.begin(), tl.end(),
                                [dst](const Tombstone& t) { return t.dst == dst; }),
                 tl.end());
    }

    uint64_t out_key = key(src, kind);
    std::vector<Edge>& out_list = outgoing_[out_key];
    auto it = std::lower_bound(out_list.begin(), out_list.end(), dst,
                               [](const Edge& e, Entity d) { return e.dst < d; });
    if (it != out_list.end() && it->dst == dst) {
        it->payload = payload; // arista existente: actualizar payload
    } else {
        out_list.insert(it, Edge{dst, payload});
    }

    uint64_t in_key = key(dst, kind);
    std::vector<BackEdge>& in_list = incoming_[in_key];
    auto bit = std::lower_bound(in_list.begin(), in_list.end(), src,
                                [](const BackEdge& e, Entity s) { return e.src < s; });
    if (bit != in_list.end() && bit->src == src) {
        bit->payload = payload;
    } else {
        in_list.insert(bit, BackEdge{src, payload});
    }

    stamp(kind, src, tick);
}

bool RelationGraph::remove_relation(Entity src, RelationKind kind, Entity dst, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    RelationPayload removed_payload;
    bool removed = false;
    uint64_t out_key = key(src, kind);
    auto it = outgoing_.find(out_key);
    if (it != outgoing_.end()) {
        auto& list = it->second;
        auto eit = std::find_if(list.begin(), list.end(),
                                [dst](const Edge& e) { return e.dst == dst; });
        if (eit != list.end()) {
            removed_payload = eit->payload;
            list.erase(eit);
            removed = true;
        }
    }

    uint64_t in_key = key(dst, kind);
    auto iit = incoming_.find(in_key);
    if (iit != incoming_.end()) {
        auto& list = iit->second;
        auto eit = std::find_if(list.begin(), list.end(),
                                [src](const BackEdge& e) { return e.src == src; });
        if (eit != list.end()) {
            list.erase(eit);
            removed = true;
        }
    }

    if (removed) {
        // Tombstone: los diffs de #7 necesitan saber qué arista se removió.
        tombstones_[out_key].push_back(Tombstone{dst, removed_payload, tick});
        stamp(kind, src, tick);
    }
    return removed;
}

void RelationGraph::remove_all_relations(Entity entity, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    // Aristas salientes: entity como src
    for (size_t k = 0; k < MAX_RELATION_KINDS; ++k) {
        auto it = outgoing_.find(key(entity, static_cast<RelationKind>(k)));
        if (it == outgoing_.end()) continue;

        for (const Edge& e : it->second) {
            uint64_t in_key = key(e.dst, static_cast<RelationKind>(k));
            auto iit = incoming_.find(in_key);
            if (iit != incoming_.end()) {
                auto& list = iit->second;
                list.erase(std::remove_if(list.begin(), list.end(),
                    [entity](const BackEdge& b) { return b.src == entity; }),
                    list.end());
            }
            tombstones_[key(entity, static_cast<RelationKind>(k))].push_back(
                Tombstone{e.dst, e.payload, tick});
            stamp(static_cast<RelationKind>(k), entity, tick);
        }
        outgoing_.erase(it);
    }

    // Aristas entrantes: entity como dst
    for (size_t k = 0; k < MAX_RELATION_KINDS; ++k) {
        auto it = incoming_.find(key(entity, static_cast<RelationKind>(k)));
        if (it == incoming_.end()) continue;

        for (const BackEdge& e : it->second) {
            uint64_t out_key = key(e.src, static_cast<RelationKind>(k));
            auto oit = outgoing_.find(out_key);
            if (oit != outgoing_.end()) {
                auto& list = oit->second;
                list.erase(std::remove_if(list.begin(), list.end(),
                    [entity](const Edge& ed) { return ed.dst == entity; }),
                    list.end());
            }
            tombstones_[out_key].push_back(Tombstone{entity, e.payload, tick});
            stamp(static_cast<RelationKind>(k), e.src, tick);
        }
        incoming_.erase(it);
    }
}

bool RelationGraph::has_relation(Entity src, RelationKind kind, Entity dst) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = outgoing_.find(key(src, kind));
    if (it == outgoing_.end()) return false;
    const auto& list = it->second;
    auto eit = std::lower_bound(list.begin(), list.end(), dst,
                                [](const Edge& e, Entity d) { return e.dst < d; });
    return eit != list.end() && eit->dst == dst;
}

bool RelationGraph::get_relation(Entity src, RelationKind kind, Entity dst, RelationPayload& out) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = outgoing_.find(key(src, kind));
    if (it == outgoing_.end()) return false;
    const auto& list = it->second;
    auto eit = std::lower_bound(list.begin(), list.end(), dst,
                                [](const Edge& e, Entity d) { return e.dst < d; });
    if (eit == list.end() || eit->dst != dst) return false;
    out = eit->payload;
    return true;
}

size_t RelationGraph::outgoing_degree(Entity src, RelationKind kind) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = outgoing_.find(key(src, kind));
    return it == outgoing_.end() ? 0 : it->second.size();
}

size_t RelationGraph::incoming_degree(Entity target, RelationKind kind) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = incoming_.find(key(target, kind));
    return it == incoming_.end() ? 0 : it->second.size();
}

bool RelationGraph::kind_changed_since(RelationKind kind, uint64_t since_tick) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (kind >= MAX_RELATION_KINDS) return false;
    return kind_rings_[kind].has_writes_since(since_tick);
}

uint32_t RelationGraph::last_write_tick(RelationKind kind, Entity src) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = last_write_ticks_.find(key(src, kind));
    return it == last_write_ticks_.end() ? 0 : it->second;
}

void RelationGraph::prune_tombstones(uint64_t before_tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    for (auto it = tombstones_.begin(); it != tombstones_.end(); ) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [before_tick](const Tombstone& t) { return t.removed_tick <= before_tick; }),
                   list.end());
        if (list.empty()) {
            it = tombstones_.erase(it);
        } else {
            ++it;
        }
    }
}

void RelationGraph::clear() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    outgoing_.clear();
    incoming_.clear();
    tombstones_.clear();
    last_write_ticks_.clear();
    for (TickRing& ring : kind_rings_) {
        ring = TickRing();
    }
}

} // namespace ecs
} // namespace fluxdb
