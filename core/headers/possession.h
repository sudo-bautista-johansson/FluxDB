#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #31: Live Server-Authoritative Entity Possession
//  (Phase 4 - Developer Experience)
// ─────────────────────────────────────────────────────────────
// Una sesión de edición en vivo es OTRO cliente del World: usa el mismo
// API de query/mutación que el gameplay, así las ediciones del dev son
// versionadas y replicables como cualquier acción de jugador (delta
// engine #7) y deshacibles vía el snapshot ring buffer (#8). Aquí se
// implementa la capa de sesión: tomar posesión (lock) de una entidad,
// aplicar edits versionados, y undo con el ring buffer del World.

#include "ecs.h"
#include "rollback.h"
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace fluxdb {
namespace dbg {

using fluxdb::ecs::World;
using fluxdb::ecs::Entity;
using fluxdb::ecs::ComponentID;

// Una edición de posesión: qué (entidad, componente) y qué tick.
struct PossessionEdit {
    Entity entity = 0;
    ComponentID comp_id = 0;
    uint64_t tick = 0;
    uint64_t seq = 0;
};

// Sesión de posesión en vivo sobre un World autoritativo.
class PossessionSession {
public:
    explicit PossessionSession(World& world) : world_(world) {}

    // Toma posesión de una entidad (debe existir).
    bool possess(Entity e) {
        if (!entity_exists(e)) return false;
        possessed_.push_back(e);
        return true;
    }

    bool is_possessed(Entity e) const {
        for (Entity p : possessed_) if (p == e) return true;
        return false;
    }

    size_t possessed_count() const { return possessed_.size(); }

    // Aplica una edición versionada (misma ruta que gameplay). Devuelve el
    // número de secuencia asignado (para undo/redo y audit trail).
    uint64_t edit(Entity e, ComponentID comp_id, const void* data, size_t size) {
        if (!is_possessed(e)) return 0;
        world_.add_component(e, comp_id, data);
        uint64_t seq = next_seq_++;
        edits_.push_back({e, comp_id, world_.current_tick(), seq});
        return seq;
    }

    // Deshace la última edición: captura el estado actual como checkpoint,
    // revierte el World al tick previo a la edición (si el ring cubre ese
    // tick). Es un undo aproximado pero determinista; el redo exacto
    // requeriría re-aplicar deltas (fuera de alcance de este demo).
    bool undo() {
        if (edits_.empty()) return false;
        uint64_t target_tick = edits_.back().tick;
        edits_.pop_back();
        return world_.rollback_to(target_tick);
    }

    // ¿Cuántas ediciones tiene la sesión (audit trail)?
    size_t edit_count() const { return edits_.size(); }

    // El último edit como (entity, comp) para inspección.
    PossessionEdit last_edit() const {
        return edits_.empty() ? PossessionEdit{} : edits_.back();
    }

    // Tick actual del mundo.
    uint64_t now() const { return world_.current_tick(); }

private:
    bool entity_exists(Entity e) const {
        for (auto& [sig, up] : world_.get_archetypes()) {
            if (up->get_entity_count() == 0) continue;
            const fluxdb::ecs::Entity* ptr = up->get_entities_ptr();
            if (!ptr) continue;
            for (size_t i = 0; i < up->get_entity_count(); ++i) {
                if (ptr[i] == e) return true;
            }
        }
        return false;
    }

    World& world_;
    std::vector<Entity> possessed_;
    std::vector<PossessionEdit> edits_;
    uint64_t next_seq_ = 1;
};

} // namespace dbg
} // namespace fluxdb
