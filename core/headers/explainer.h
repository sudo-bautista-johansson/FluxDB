#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #29: Visual Query Plan Explainer
//  (Phase 4 - Developer Experience)
// ─────────────────────────────────────────────────────────────
// API introspectivo `query.explain()` sobre los planes compilados (#5):
// dumps el conjunto de arquetipos que matchean, cuentas estimadas de
// entidades, componentes requeridos en orden de evaluación y flag de
// patrones lentos (por ejemplo, un plan que matchea muchos arquetipos
// con pocas entidades = barrido disperso). Cero coste cuando no se usa:
// es una llamada de diagnóstico en dev-time.

#include "ecs.h"
#include "query_plans.h"
#include <string>
#include <vector>
#include <sstream>
#include <cmath>

namespace fluxdb {
namespace dbg {

using fluxdb::ecs::World;
using fluxdb::ecs::QueryHandle;
using fluxdb::ecs::QueryPlan;

// Línea del explain: descripción de una parte del plan.
struct ExplainLine {
    std::string text;
    bool warning = false;   // marcado como patrón lento
};

struct ExplainResult {
    QueryHandle handle = 0;
    std::vector<fluxdb::ecs::ComponentID> components;
    size_t matched_archetypes = 0;
    size_t estimated_entities = 0;
    size_t estimated_cost = 0; // entidades × arquetipos matcheados
    bool slow_pattern = false;
    std::vector<ExplainLine> lines;
};

class QueryExplainer {
public:
    explicit QueryExplainer(const World& world) : world_(world) {}

    // Explica un handle de query compilada.
    ExplainResult explain(QueryHandle handle) const {
        ExplainResult res;
        res.handle = handle;

        const QueryPlan* plan = world_.get_query_plan(handle);
        if (!plan) {
            res.lines.push_back({"INVALID query handle", true});
            res.slow_pattern = true;
            return res;
        }

        res.components = plan->components();
        res.matched_archetypes = plan->matched_archetype_count();

        std::stringstream ss;
        ss << "Query #" << handle << " components: {";
        for (size_t i = 0; i < res.components.size(); ++i) {
            if (i) ss << ", ";
            ss << res.components[i];
        }
        ss << "}";
        res.lines.push_back({ss.str(), false});

        ss.str("");
        ss << "Matched archetypes: " << res.matched_archetypes;
        res.lines.push_back({ss.str(), false});

        // Estimación de entidades y coste recorriendo los arquetipos.
        size_t entities = 0;
        size_t cost = 0;
        for (const auto& [sig, up] : world_.get_archetypes()) {
            if (plan->matches_archetype(up->get_signature())) {
                size_t count = up->get_entity_count();
                entities += count;
                cost += count * res.matched_archetypes;
                ss.str("");
                ss << "  -> archetype sig=" << sig
                   << " entities=" << count;
                res.lines.push_back({ss.str(), count < 4 && count > 0});
            }
        }
        res.estimated_entities = entities;
        res.estimated_cost = cost;

        // Patrones lentos:
        //  - 0 entidades → query muerta.
        //  - cost > 10000 → barrido amplio sobre muchas filas.
        //  - muchos arquetipos para pocas entidades → fragmentación.
        if (entities == 0) {
            res.lines.push_back({"WARNING: query matches 0 entities (dead query)", true});
            res.slow_pattern = true;
        }
        if (res.matched_archetypes > 4) {
            ss.str("");
            ss << "WARNING: " << res.matched_archetypes
               << " archetypes matched — potential archetype fragmentation";
            res.lines.push_back({ss.str(), true});
            res.slow_pattern = true;
        }
        if (cost > 10000) {
            ss.str("");
            ss << "WARNING: estimated cost " << cost << " (>10000) — expensive scan";
            res.lines.push_back({ss.str(), true});
            res.slow_pattern = true;
        }
        return res;
    }

private:
    const World& world_;
};

} // namespace dbg
} // namespace fluxdb
