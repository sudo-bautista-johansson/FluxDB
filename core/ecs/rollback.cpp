#include "../headers/rollback.h"

namespace fluxdb {
namespace rollback {

// ── WorldSnapshot ─────────────────────────────────────────────

void WorldSnapshot::capture(const ecs::World& world) {
    set_ = delta::capture_world_snapshot(world);
    captured_ = true;
}

void WorldSnapshot::restore(ecs::World& world) const {
    world.clear_all();
    // El reloj se fija ANTES de aplicar para que los stamps de versión (#4)
    // queden en el tick original del snapshot.
    world.advance_to(set_.base_tick());
    set_.apply(world);
}

// ── SnapshotRingBuffer ────────────────────────────────────────

SnapshotRingBuffer::SnapshotRingBuffer(size_t capacity) : capacity_(capacity) {}

void SnapshotRingBuffer::capture(ecs::World& world) {
    uint64_t tick = world.current_tick();

    if (!base_captured_) {
        base_.capture(world);
        world.capture_chunk_pages(base_pages_); // (#8) COW fast path
        base_captured_ = true;
        last_tick_ = tick;
        return;
    }

    if (tick == last_tick_) {
        return; // el tick no avanzó: nada nuevo que registrar
    }

    delta::DeltaSet d = delta::capture_tick_delta(world, last_tick_);
    d.set_end_tick(tick);
    deltas_.push_back(std::move(d));

    if (deltas_.size() > capacity_) {
        deltas_.erase(deltas_.begin());
    }
    last_tick_ = tick;
}

void SnapshotRingBuffer::clear() {
    base_ = WorldSnapshot();
    base_pages_ = ecs::ChunkPageSnapshot();
    base_captured_ = false;
    deltas_.clear();
    last_tick_ = 0;
}

uint64_t SnapshotRingBuffer::base_tick() const {
    return base_.tick();
}

uint64_t SnapshotRingBuffer::latest_tick() const {
    return deltas_.empty() ? base_.tick() : deltas_.back().end_tick();
}

bool SnapshotRingBuffer::covers_tick(uint64_t tick) const {
    if (!base_captured_) return false;
    if (tick == base_.tick()) return true;
    if (tick < base_.tick() || tick > latest_tick()) return false;
    for (const auto& d : deltas_) {
        if (d.end_tick() == tick) return true;
    }
    return false;
}

} // namespace rollback
} // namespace fluxdb
