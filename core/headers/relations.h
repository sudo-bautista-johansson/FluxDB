#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include "versioning.h"

namespace fluxdb {
namespace ecs {

using Entity = uint32_t;
using RelationKind = uint8_t;

// ─────────────────────────────────────────
//  Native Entity Relationship Graphs (#6)
// ─────────────────────────────────────────
// Las relaciones son un tipo de arista de primera clase: tripletas
// (src, kind, dst) indexadas hacia adelante y hacia atrás en estructuras
// de adyacencia compactas co-localizadas con el World (no un grafo aparte).
// Cada arista puede llevar un payload pequeño (offset de socket, peso, etc).
// Los cambios de relación se sellan con el mismo sistema de versionado (#4):
// TickRing grueso por kind + tick fino por (src, kind), listos para que el
// motor de deltas (#7) los convierta en eventos replicables.

constexpr size_t MAX_RELATION_KINDS = 32;

// Payload de arista: hasta 8 bytes raw (p.ej. offset de socket).
struct RelationPayload {
    uint64_t raw = 0;

    template <typename T>
    T as() const {
        static_assert(sizeof(T) <= sizeof(uint64_t), "payload max 8 bytes");
        T v;
        std::memcpy(&v, &raw, sizeof(T));
        return v;
    }

    template <typename T>
    void set_as(T v) {
        static_assert(sizeof(T) <= sizeof(uint64_t), "payload max 8 bytes");
        std::memcpy(&raw, &v, sizeof(T));
    }

    static RelationPayload from(uint64_t r) {
        RelationPayload p;
        p.raw = r;
        return p;
    }
};

class RelationGraph {
public:
    struct Edge { Entity dst; RelationPayload payload; };
    struct BackEdge { Entity src; RelationPayload payload; };

    // Añade o actualiza (payload) la arista (src, kind, dst).
    // `tick` > 0 sella la versión (fino por (src,kind) + grueso por kind).
    void add_relation(Entity src, RelationKind kind, Entity dst, RelationPayload payload = {}, uint64_t tick = 0);

    // Elimina la arista. Devuelve false si no existía. `tick` > 0 sella.
    bool remove_relation(Entity src, RelationKind kind, Entity dst, uint64_t tick = 0);

    // Elimina TODAS las aristas de `entity` (como src y como dst).
    // Se invoca desde World::despawn (#6). `tick` > 0 sella.
    void remove_all_relations(Entity entity, uint64_t tick = 0);

    bool has_relation(Entity src, RelationKind kind, Entity dst) const;

    bool get_relation(Entity src, RelationKind kind, Entity dst, RelationPayload& out) const;

    size_t outgoing_degree(Entity src, RelationKind kind) const;
    size_t incoming_degree(Entity target, RelationKind kind) const;

    // Forward: f(dst, payload) por cada arista saliente de `src` en `kind`.
    // Backward O(degree): f(src, payload) por cada arista entrante a `target`.
    template <typename F>
    void for_each_outgoing(Entity src, RelationKind kind, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = outgoing_.find(key(src, kind));
        if (it == outgoing_.end()) return;
        for (const Edge& e : it->second) {
            f(e.dst, e.payload);
        }
    }

    template <typename F>
    void for_each_incoming(Entity target, RelationKind kind, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = incoming_.find(key(target, kind));
        if (it == incoming_.end()) return;
        for (const BackEdge& e : it->second) {
            f(e.src, e.payload);
        }
    }

    // Todas las aristas vivas: f(src, kind, dst, payload).
    // Usado por snapshots de estado completo (#7).
    template <typename F>
    void for_each_edge(F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        for (const auto& [k, list] : outgoing_) {
            Entity src = static_cast<Entity>(k >> 8);
            RelationKind kind = static_cast<RelationKind>(k & 0xFF);
            for (const Edge& e : list) {
                f(src, kind, e.dst, e.payload);
            }
        }
    }

    // ── Versioning (#4) ──

    // Coarse O(1): ¿hubo cambios de relaciones de `kind` después de `since_tick`?
    bool kind_changed_since(RelationKind kind, uint64_t since_tick) const;

    // Fino: tick del último cambio (src, kind). 0 = nunca.
    uint32_t last_write_tick(RelationKind kind, Entity src) const;

    // Iteración para capturas de diff (#7): f(src) por cada (src, kind) cuyo
    // último cambio fue estrictamente después de `since_tick`.
    template <typename F>
    void for_each_changed_src(RelationKind kind, uint64_t since_tick, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        for (const auto& [key, tick] : last_write_ticks_) {
            if (static_cast<uint8_t>(key & 0xFF) != kind) continue;
            if (tick > since_tick) {
                f(static_cast<Entity>(key >> 8));
            }
        }
    }

    // ── Removals (#6/#7) ──

    // Tombstones de aristas removidas: el grafo solo guarda aristas vivas,
    // así que los diffs de replicación (#7) necesitan saber QUÉ se removió.
    // Las tumbas viven hasta prune_tombstones(before_tick).

    struct Tombstone { Entity dst; RelationPayload payload; uint64_t removed_tick; };

    // f(src, dst, payload) por cada arista removida estrictamente después de `since_tick`.
    template <typename F>
    void for_each_removed_since(RelationKind kind, uint64_t since_tick, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        for (const auto& [key, list] : tombstones_) {
            if (static_cast<uint8_t>(key & 0xFF) != kind) continue;
            Entity src = static_cast<Entity>(key >> 8);
            for (const Tombstone& t : list) {
                if (t.removed_tick > since_tick) {
                    f(src, t.dst, t.payload);
                }
            }
        }
    }

    // Descarta tumbas removidas en o antes de `before_tick`.
    void prune_tombstones(uint64_t before_tick);

    // Reset total (World::clear_all, #8).
    void clear();

private:
    static uint64_t key(Entity e, RelationKind k) {
        return (static_cast<uint64_t>(e) << 8) | static_cast<uint64_t>(k & 0xFF);
    }

    void stamp(RelationKind kind, Entity src, uint64_t tick);

    // key(src, kind) -> aristas salientes (ordenadas por dst)
    std::unordered_map<uint64_t, std::vector<Edge>> outgoing_;
    // key(dst, kind) -> aristas entrantes
    std::unordered_map<uint64_t, std::vector<BackEdge>> incoming_;

    // key(src, kind) -> tumbas de aristas removidas
    std::unordered_map<uint64_t, std::vector<Tombstone>> tombstones_;

    std::unordered_map<uint64_t, uint32_t> last_write_ticks_;
    TickRing kind_rings_[MAX_RELATION_KINDS];

    mutable std::shared_mutex rw_mutex_;
};

} // namespace ecs
} // namespace fluxdb
