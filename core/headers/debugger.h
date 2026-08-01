#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #27: Time-Travel World Debugger
//  (Phase 4 - Developer Experience)
// ─────────────────────────────────────────────────────────────
// Consumidor READ-ONLY de la infraestructura existente (#4 versioning,
// #7 delta, #8 rollback/history): un timeline scrubber que reconstruye
// el estado exacto de cualquier (entidad, componente) en un tick pasado,
// diffs entre ticks, y atribuye escrituras (qué tick / qué entidad /
// qué componente). Sin overhead cuando no se usa: solo lee HistoryManager
// y VersionTracker.

#include "ecs.h"
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

namespace fluxdb {
namespace dbg {

using fluxdb::ecs::Entity;
using fluxdb::ecs::World;

// Valor de un componente en un tick histórico.
struct DebuggedValue {
    bool found = false;
    uint64_t tick = 0;
    Entity entity = 0;
    fluxdb::ecs::ComponentID comp_id = 0;
    std::vector<uint8_t> bytes;
};

// Un cambio registrado entre ticks (para el diff del timeline).
struct DebugChange {
    Entity entity = 0;
    fluxdb::ecs::ComponentID comp_id = 0;
    uint64_t tick = 0;
};

// Atribución de escritura: qué sistema escribió (representada aquí por
// el tick y la entidad/componente; la atribución por sistema exacta es
// responsabilidad del caller con sistemas nombrados).
struct DebugWrite {
    Entity entity = 0;
    fluxdb::ecs::ComponentID comp_id = 0;
    uint64_t tick = 0;
    uint32_t last_write_tick = 0;
};

class TimeTravelDebugger {
public:
    explicit TimeTravelDebugger(const World& world) : world_(&world) {}

    // Reconstruye el estado de (entity, comp) en un tick pasado exacto.
    DebuggedValue state_at(uint64_t tick, Entity entity, fluxdb::ecs::ComponentID comp_id) const {
        DebuggedValue out;
        out.tick = tick;
        out.entity = entity;
        out.comp_id = comp_id;

        size_t size = 0;
        const void* current = world_->get_entity_component_data(entity, comp_id, size);
        if (!current) return out;

        out.bytes.resize(size);
        if (world_->get_history() &&
            world_->get_history()->get_historical_state(tick, entity, comp_id, out.bytes.data(), size)) {
            out.found = true;
            return out;
        }

        // Fallback al valor live (si el tick >= now, o sin historial).
        out.found = true;
        std::memcpy(out.bytes.data(), current, size);
        return out;
    }

    // Todos los cambios ocurridos entre from_tick y to_tick (únicos).
    std::vector<DebugChange> diff(uint64_t from_tick, uint64_t to_tick) const {
        std::vector<DebugChange> out;
        if (!world_->get_history()) return out;

        std::set<std::pair<uint32_t, uint8_t>> dirty;
        world_->get_history()->get_modifications_since(from_tick, dirty);
        for (const auto& [e, c] : dirty) {
            DebugChange dc;
            dc.entity = e;
            dc.comp_id = c;
            dc.tick = to_tick;
            out.push_back(dc);
        }
        return out;
    }

    // Tick actual del mundo.
    uint64_t now() const { return world_->current_tick(); }

    // Atribución: último tick de escritura del componente (versioning #4).
    DebugWrite last_write(Entity entity, fluxdb::ecs::ComponentID comp_id) const {
        DebugWrite w;
        w.entity = entity;
        w.comp_id = comp_id;
        w.last_write_tick = world_->entity_last_write_tick(entity, comp_id);
        return w;
    }

    // ¿El componente cambió entre dos ticks?
    bool changed_between(Entity entity, fluxdb::ecs::ComponentID comp_id,
                         uint64_t from_tick, uint64_t to_tick) const {
        DebuggedValue a = state_at(from_tick, entity, comp_id);
        DebuggedValue b = state_at(to_tick, entity, comp_id);
        if (!a.found || !b.found) return a.found != b.found;
        return a.bytes != b.bytes;
    }

    // Scrub de timeline: reconstruye en lote un componente de una entidad
    // en cada tick de [from..to] para animar el estado.
    std::vector<DebuggedValue> scrub(Entity entity, fluxdb::ecs::ComponentID comp_id,
                                     uint64_t from, uint64_t to) const {
        std::vector<DebuggedValue> out;
        for (uint64_t t = from; t <= to; ++t) {
            out.push_back(state_at(t, entity, comp_id));
        }
        return out;
    }

    // Punto de divergencia: primer tick donde dos snapshots difieren.
    // Útil para desync client/server (#27 multiplayer).
    uint64_t first_divergence(Entity entity, fluxdb::ecs::ComponentID comp_id,
                              const World& other, uint64_t from, uint64_t to) const {
        TimeTravelDebugger od(other);
        for (uint64_t t = from; t <= to; ++t) {
            DebuggedValue a = state_at(t, entity, comp_id);
            DebuggedValue b = od.state_at(t, entity, comp_id);
            if (a.found != b.found) return t;
            if (a.found && a.bytes != b.bytes) return t;
        }
        return to + 1; // sin divergencia
    }

private:
    const World* world_;
};

} // namespace dbg
} // namespace fluxdb
