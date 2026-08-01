#pragma once

// ─────────────────────────────────────────────────────────────
//  First-Class Rollback Netcode (#8) — Snapshot Ring Buffer
// ─────────────────────────────────────────────────────────────
// GGPO-style rollback as a database primitive:
//
//   SnapshotRingBuffer retains a base snapshot + the last N ticks of
//   DeltaSets (the exact same unified format as #7). The base is captured
//   TWICE: as a DeltaSet (portable, para save/replay) y como un COW
//   structural snapshot (ChunkPageSnapshot): las páginas de chunks del
//   storage se COMPARTEN por puntero — los writes posteriores las clonan
//   (copy-on-write). Rollback = repuntar los punteros activos a la versión
//   histórica + re-aplicar los deltas hacia adelante.
//
// Canonical flow (per server tick):
//   world.advance_tick();
//   systems_do_mutations(world);
//   ring.capture(world);
//
// On prediction mismatch:
//   world.rollback_to(latest_confirmed_tick);   // exact state at that tick
//   ...apply corrected inputs / resimulate...
//   world.resimulate(from_tick, to_tick);       // re-applies recorded deltas
//   world.resimulate(from_tick, to_tick, inputs); // ...con inputs que CORRIGEN
//
// COW chunk pages (the "very hard" allocator part of the roadmap): DONE —
// el payload de componentes vive en páginas de Archetype::PAGE_ROWS filas;
// el snapshot las comparte y ensure_owned() las clona al escribir.

#include <cstdint>
#include <cstddef>
#include <vector>
#include "delta_set.h"

namespace fluxdb {
namespace rollback {

// Input externo re-aplicado durante la resimulación (#8). Modela "lo que el
// jugador/cliente pidió en el tick": corrección GGPO-style que se aplica
// DESPUÉS del delta grabado de su tick y gana sobre él.
enum class InputOp : uint8_t {
    SET_COMPONENT = 0,   // escribe el componente (overwrite, idempotente)
    ADD_RELATION = 1,    // arista (src, kind, dst) + payload (#6)
    REMOVE_RELATION = 2, // quita la arista
};

struct ExternalInput {
    uint64_t tick = 0;                  // tick al que pertenece el input
    InputOp op = InputOp::SET_COMPONENT;
    ecs::Entity entity = 0;
    ecs::ComponentID comp_id = 0;       // SET_COMPONENT
    std::vector<uint8_t> data;          // SET_COMPONENT: bytes crudos
    ecs::RelationKind kind = 0;         // ADD/REMOVE_RELATION
    ecs::Entity dst = 0;
    ecs::RelationPayload payload;       // ADD_RELATION
};

// Captura inmutable del estado completo de un World en un tick.
// Se construye con capture() y se re-aplica con restore().
class WorldSnapshot {
public:
    WorldSnapshot() = default;

    // Tick en el que fue capturado.
    uint64_t tick() const { return set_.base_tick(); }

    bool captured() const { return captured_; }

    // Captura el estado completo del world (SPAWN records, formato #7).
    void capture(const ecs::World& world);

    // Reconstruye el world a este estado exacto (clear + apply + advance_to).
    void restore(ecs::World& world) const;

    const delta::DeltaSet& set() const { return set_; }

private:
    delta::DeltaSet set_;
    bool captured_ = false;
};

// Retiene un snapshot base + los últimos `capacity` ticks de deltas.
// El rango de rollback válido es [base_tick, latest_tick].
class SnapshotRingBuffer {
public:
    explicit SnapshotRingBuffer(size_t capacity = 64);

    // Registra el estado actual del world: la primera llamada captura el
    // snapshot base; las siguientes capturan el delta del tick. Si el tick
    // no avanzó desde la última captura, es no-op.
    void capture(ecs::World& world);

    void clear();

    bool has_base() const { return base_captured_; }
    uint64_t base_tick() const;
    uint64_t latest_tick() const;
    size_t size() const { return deltas_.size(); }
    size_t capacity() const { return capacity_; }

    const WorldSnapshot& base() const { return base_; }
    const std::vector<delta::DeltaSet>& deltas() const { return deltas_; }

    // (#8) Base COW: páginas de chunks compartidas (fast path en memoria).
    // Se captura junto con la base DeltaSet y es la que usa rollback_to.
    const ecs::ChunkPageSnapshot& base_pages() const { return base_pages_; }

    // True si el tick es exactamente alcanzable (base o fin de un delta).
    bool covers_tick(uint64_t tick) const;

private:
    size_t capacity_;
    WorldSnapshot base_;
    ecs::ChunkPageSnapshot base_pages_; // (#8) COW fast path
    bool base_captured_ = false;
    std::vector<delta::DeltaSet> deltas_;
    uint64_t last_tick_ = 0;
};

} // namespace rollback
} // namespace fluxdb
