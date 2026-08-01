#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace fluxdb {
namespace ecs {

// ─────────────────────────────────────────
//  Rolling ring buffer of version stamps
// ─────────────────────────────────────────
// Coarse-grained change tracking for a component array ("chunk" level):
// remembers the last CAPACITY ticks in which ANY write happened to the
// array, so queries can skip whole arrays in O(1) when nothing changed.
// Stamps older than the window are dropped; per-entity version ticks
// remain the exact fine-grained source of truth.
class TickRing {
public:
    static constexpr size_t CAPACITY = 64;

    // Registers a write at `tick` (deduplicated against the newest stamp).
    void mark(uint64_t tick);

    // True if any write happened strictly AFTER `since_tick`.
    // Returns false when the ring no longer covers that range (no evidence).
    bool has_writes_since(uint64_t since_tick) const;

    // Most recent tick with a write, or 0 if the ring is empty.
    uint64_t last_write_tick() const { return count_ == 0 ? 0 : stamps_[(head_ + count_ - 1) % CAPACITY]; }

    size_t count() const { return count_; }

private:
    uint64_t stamps_[CAPACITY] = {};
    size_t head_ = 0;
    size_t count_ = 0;
};

// ─────────────────────────────────────────
//  Chunk-granularity change tracking (#4)
// ─────────────────────────────────────────
// Partitions a component array into fixed-size row ranges ("chunks") and
// keeps one TickRing per chunk. Queries can skip an entire chunk in O(1)
// when it has no writes since a reference tick, and only walk per-entity
// ticks inside dirty chunks. The coarse array-level ring (dirty_rings_)
// remains the O(1) whole-array filter; this tracker is the second,
// chunk-level filter of the "coarse → fine" cascade.
class ChunkedDirtyTracker {
public:
    static constexpr size_t CHUNK_SIZE = 256;

    // Registers a write at `row` / `tick` (routes to the owning chunk ring).
    void mark(size_t row, uint64_t tick);

    // O(1): did the chunk have any write strictly after `since_tick`?
    // False when the chunk ring is empty or no longer covers that range.
    bool chunk_has_writes_since(size_t chunk_idx, uint64_t since_tick) const;

    // Conservative: any write anywhere in the array since `since_tick`.
    bool has_any_write_since(uint64_t since_tick) const;

    // Marks every allocated chunk ring at `tick` (structural change).
    void mark_all(uint64_t tick);

    // Number of chunk rings currently allocated (may exceed the live row
    // count after a shrink; extra rings are simply never queried).
    size_t chunk_count() const { return rings_.size(); }

    // Ensures enough rings to cover `num_entities` rows.
    void ensure_size(size_t num_entities);

    // Most recent write tick across all chunks (0 when nothing recorded).
    uint64_t last_write_tick() const;

private:
    std::vector<TickRing> rings_;
};

// ─────────────────────────────────────────
//  Simulation clock (tick source of truth)
// ─────────────────────────────────────────
// Owned by World. When a HistoryManager is attached, World::current_tick()
// prefers the history tick so both subsystems always stamp the same value.
class VersionTracker {
public:
    uint64_t current_tick() const { return tick_; }
    void advance_tick() { ++tick_; }
    void advance_to(uint64_t t) { tick_ = t; }

private:
    uint64_t tick_ = 0;
};

} // namespace ecs
} // namespace fluxdb
