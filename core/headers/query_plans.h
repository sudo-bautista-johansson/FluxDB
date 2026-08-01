#pragma once

#include "ecs.h"
#include <utility>
#include <shared_mutex>

namespace fluxdb {
namespace ecs {

// ─────────────────────────────────────────
//  Compiled Query Plans (#5)
// ─────────────────────────────────────────
// Un QueryPlan compila una consulta por firma de componentes una sola vez:
// - lista precomputada de arquetipos que matchean (no se re-resuelve por frame)
// - strides por (arquetipo, componente) precomputados
// - invalidación selectiva: al crearse un arquetipo nuevo solo se actualizan
//   los planes cuya firma matchea (via get_or_create_archetype -> add_archetype)
//
// Nota de concurrencia: for_each* requieren que el llamador sostenga el
// shared lock del World (garantiza que ningún add/remove estructural ocurra
// mientras se itera). No llames world.add_component/despawn desde el callback.

using QueryHandle = uint32_t;

struct QueryAccessor {
    ComponentID comp_id;
    uint32_t stride;   // bytes por entidad (precomputado)
    uint8_t* array;    // base del array contiguo (refrescado por arquetipo)
};

// Fila de una consulta: acceso a columnas sin locks por fila (offsets precomputados).
struct QueryRow {
    const QueryAccessor* accessors;
    uint32_t num_accessors;
    size_t row;

    const void* get(ComponentID comp_id) const {
        for (uint32_t i = 0; i < num_accessors; ++i) {
            if (accessors[i].comp_id == comp_id) {
                return accessors[i].array + row * accessors[i].stride;
            }
        }
        return nullptr;
    }
};

class QueryPlan {
public:
    QueryPlan(std::vector<ComponentID> required, const ComponentStore& store);

    const std::vector<ComponentID>& components() const { return components_; }

    // ¿La firma del arquetipo contiene TODOS los componentes requeridos?
    bool matches_archetype(const ArchetypeSignature& sig) const {
        return (sig & signature_) == signature_;
    }

    // Agrega un arquetipo a los matches (con offsets precomputados).
    // Se invoca al crearse arquetipos nuevos (invalidación selectiva).
    void add_archetype(Archetype* arch, const ComponentStore& store);

    // Remueve un arquetipo de los matches (invalidación por remoción).
    // Se invoca desde World::remove_archetype (#5).
    void remove_archetype(Archetype* arch);

    size_t matched_archetype_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return matches_.size();
    }

    // Versión del plan; se incrementa en cada add_archetype (los matches cambiaron).
    uint64_t plan_version() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return version_;
    }

    // Iteración: f(entity, row, QueryRow). O(matches) sin locks por fila.
    template <typename F>
    void for_each(F&& f) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const MatchedArchetype& m : matches_) {
            Archetype* arch = m.archetype;
            size_t count = arch->get_entity_count();
            const Entity* entities = arch->get_entities_ptr();
            if (count == 0 || !entities) continue;

            std::vector<QueryAccessor> accs = m.accessors;
            for (QueryAccessor& acc : accs) {
                acc.array = arch->get_component_array(acc.comp_id);
            }

            for (size_t row = 0; row < count; ++row) {
                QueryRow qr{accs.data(), static_cast<uint32_t>(accs.size()), row};
                f(entities[row], row, qr);
            }
        }
    }

    // Iteración con filtro temporal (#4): solo entidades cuyo último write del
    // componente `comp_id` fue estrictamente después de `since_tick`.
    template <typename F>
    void for_each_changed(ComponentID comp_id, uint64_t since_tick, F&& f) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const MatchedArchetype& m : matches_) {
            Archetype* arch = m.archetype;
            const uint32_t* ticks = arch->get_last_write_ticks_ptr(comp_id);
            if (!ticks) continue;

            size_t count = arch->get_entity_count();
            const Entity* entities = arch->get_entities_ptr();
            if (count == 0 || !entities) continue;

            std::vector<QueryAccessor> accs = m.accessors;
            for (QueryAccessor& acc : accs) {
                acc.array = arch->get_component_array(acc.comp_id);
            }

            for (size_t row = 0; row < count; ++row) {
                if (ticks[row] > since_tick) {
                    QueryRow qr{accs.data(), static_cast<uint32_t>(accs.size()), row};
                    f(entities[row], row, qr);
                }
            }
        }
    }

private:
    struct MatchedArchetype {
        Archetype* archetype;
        std::vector<QueryAccessor> accessors;
    };

    std::vector<ComponentID> components_;
    ArchetypeSignature signature_;
    std::vector<MatchedArchetype> matches_;
    mutable std::shared_mutex mutex_;
    uint64_t version_ = 0;
};

// ─────────────────────────────────────────
//  Convenience API sobre World (requiere query_plans.h)
// ─────────────────────────────────────────

// Itera el plan tomando el shared lock del World (consistencia estructural).
template <typename F>
void for_each_in_query(World& world, QueryHandle handle, F&& f) {
    std::shared_lock<std::shared_mutex> lock(world.get_mutex());
    const QueryPlan* plan = world.get_query_plan(handle);
    if (plan) {
        plan->for_each(std::forward<F>(f));
    }
}

// Itera el plan con filtro temporal: solo entidades cambiadas tras since_tick.
template <typename F>
void for_each_changed_in_query(World& world, QueryHandle handle, ComponentID comp_id, uint64_t since_tick, F&& f) {
    std::shared_lock<std::shared_mutex> lock(world.get_mutex());
    const QueryPlan* plan = world.get_query_plan(handle);
    if (plan) {
        plan->for_each_changed(comp_id, since_tick, std::forward<F>(f));
    }
}

} // namespace ecs
} // namespace fluxdb
