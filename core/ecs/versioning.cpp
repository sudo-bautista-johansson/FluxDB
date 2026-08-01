#include "../headers/versioning.h"

namespace fluxdb {
namespace ecs {

void TickRing::mark(uint64_t tick) {
    if (count_ == 0) {
        stamps_[0] = tick;
        head_ = 0;
        count_ = 1;
        return;
    }

    uint64_t newest = stamps_[(head_ + count_ - 1) % CAPACITY];
    if (tick == newest) {
        return; // Mismo tick, no duplicamos la marca
    }

    if (count_ < CAPACITY) {
        size_t idx = (head_ + count_) % CAPACITY;
        stamps_[idx] = tick;
        count_++;
        return;
    }

    // Ring lleno: sobreescribimos el stamp más viejo (el nuevo es el head)
    stamps_[head_] = tick;
    head_ = (head_ + 1) % CAPACITY;
}

bool TickRing::has_writes_since(uint64_t since_tick) const {
    for (size_t i = 0; i < count_; ++i) {
        if (stamps_[i] > since_tick) {
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────
//  ChunkedDirtyTracker (#4)
// ─────────────────────────────────────────

void ChunkedDirtyTracker::mark(size_t row, uint64_t tick) {
    size_t chunk = row / CHUNK_SIZE;
    if (chunk >= rings_.size()) {
        rings_.resize(chunk + 1);
    }
    rings_[chunk].mark(tick);
}

bool ChunkedDirtyTracker::chunk_has_writes_since(size_t chunk_idx, uint64_t since_tick) const {
    if (chunk_idx >= rings_.size()) {
        return false;
    }
    return rings_[chunk_idx].has_writes_since(since_tick);
}

bool ChunkedDirtyTracker::has_any_write_since(uint64_t since_tick) const {
    for (const TickRing& ring : rings_) {
        if (ring.has_writes_since(since_tick)) {
            return true;
        }
    }
    return false;
}

void ChunkedDirtyTracker::mark_all(uint64_t tick) {
    for (TickRing& ring : rings_) {
        ring.mark(tick);
    }
}

void ChunkedDirtyTracker::ensure_size(size_t num_entities) {
    size_t needed = (num_entities + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (needed > rings_.size()) {
        rings_.resize(needed);
    }
}

uint64_t ChunkedDirtyTracker::last_write_tick() const {
    uint64_t newest = 0;
    for (const TickRing& ring : rings_) {
        uint64_t t = ring.last_write_tick();
        if (t > newest) newest = t;
    }
    return newest;
}

} // namespace ecs
} // namespace fluxdb
