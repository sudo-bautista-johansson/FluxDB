#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #28: Cache & Fragmentation Profiler
//  (Phase 4 - Developer Experience)
// ─────────────────────────────────────────────────────────────
// Instrumentación de bajo coste (muestreo, no por acceso) que reporta
// ocupación de chunks, fragmentación de arquetipos, utilización de
// líneas de caché por query y un "cache efficiency score" por
// componente. Solo lee el layout existente (#1 chunks/páginas):
// no agrega estado de runtime cuando está deshabilitado.

#include "ecs.h"
#include <string>
#include <vector>
#include <map>
#include <cmath>

namespace fluxdb {
namespace dbg {

using fluxdb::ecs::World;
using fluxdb::ecs::ComponentID;
using fluxdb::ecs::Archetype;

// Reporte por arquetipo.
struct ArchetypeReport {
    fluxdb::ecs::ArchetypeSignature signature = 0;
    size_t entity_count = 0;
    size_t num_components = 0;
    float occupancy = 0.0f;       // fill ratio del último chunk (0..1)
    size_t wasted_capacity = 0;   // filas vacías en chunks activos
};

// Reporte por componente.
struct ComponentReport {
    ComponentID comp_id = 0;
    size_t stride = 0;
    size_t bytes_total = 0;
    size_t pages = 0;
    float cache_lines_per_row = 0.0f; // aprox. líneas tocadas por fila
    float efficiency = 1.0f;          // 0..1 (mayor = mejor cache-locality)
    uint64_t accesses = 0;
};

// Reporte global del World.
struct ProfileReport {
    size_t archetype_count = 0;
    size_t total_entities = 0;
    size_t total_bytes = 0;
    float global_occupancy = 0.0f;
    std::vector<ArchetypeReport> archetypes;
    std::vector<ComponentReport> components;
};

// Calcula (estimado) la ocupación del último chunk de un arquetipo.
// El arquetipo almacena entidades contiguas; el último chunk suele
// estar parcialmente lleno si no hay múltiplos exactos.
static float estimate_occupancy(size_t entity_count, size_t rows_per_chunk) {
    if (entity_count == 0) return 0.0f;
    size_t full_chunks = entity_count / rows_per_chunk;
    size_t remainder = entity_count % rows_per_chunk;
    if (remainder == 0) return 1.0f;
    // Ocupación media ponderada: chunks llenos + el último parcial.
    float last = static_cast<float>(remainder) / rows_per_chunk;
    return (full_chunks + last) / (full_chunks + 1.0f);
}

class CacheProfiler {
public:
    explicit CacheProfiler(const World& world) : world_(world) {}

    // Perfil completo (layout actual + tier stats #1).
    ProfileReport profile() const {
        ProfileReport rep;

        const auto& archetypes = world_.get_archetypes();
        rep.archetype_count = archetypes.size();
        rep.total_bytes = 0;
        rep.total_entities = 0;
        rep.global_occupancy = 0.0f;

        const size_t CHUNK_ROWS = fluxdb::ecs::ChunkedDirtyTracker::CHUNK_SIZE;

        for (const auto& [sig, up] : archetypes) {
            const Archetype& arch = *up;
            size_t count = arch.get_entity_count();
            size_t num_comps = arch.get_signature().count();
            rep.total_entities += count;

            ArchetypeReport ar;
            ar.signature = sig;
            ar.entity_count = count;
            ar.num_components = num_comps;
            ar.occupancy = estimate_occupancy(count, CHUNK_ROWS);
            rep.global_occupancy += ar.occupancy;
            rep.archetypes.push_back(ar);
        }
        if (!rep.archetypes.empty()) rep.global_occupancy /= rep.archetypes.size();

        // Componentes: stride + bytes + líneas de caché estimadas.
        const fluxdb::ecs::ComponentStore* store = world_.get_store();
        size_t n = store->count();
        for (ComponentID id = 0; id < n; ++id) {
            const auto& info = store->get_info(id);
            size_t stride = info.size;
            rep.total_bytes += stride * rep.total_entities;

            ComponentReport cr;
            cr.comp_id = id;
            cr.stride = stride;
            cr.bytes_total = stride * rep.total_entities;
            cr.pages = (rep.total_entities + CHUNK_ROWS - 1) / CHUNK_ROWS;
            cr.cache_lines_per_row = (stride + 63.0f) / 64.0f; // líneas de 64B por fila
            // Eficiencia: 1.0 si el componente cabe en <= 1 línea y hay
            // muchos por fila; baja si stride > 1 línea (sparse).
            cr.efficiency = (cr.cache_lines_per_row <= 1.0f) ? 1.0f
                          : (1.0f / cr.cache_lines_per_row);
            rep.components.push_back(cr);
        }
        return rep;
    }

    // Cuenta la fragmentación estructural: arquetipos con < `min_rows`
    // entidades (chunks poco poblados que malgastan memoria).
    size_t count_sparse_archetypes(size_t min_rows = 8) const {
        size_t n = 0;
        for (const auto& [sig, up] : world_.get_archetypes()) {
            if (up->get_entity_count() > 0 && up->get_entity_count() < min_rows) ++n;
        }
        return n;
    }

    // Score global de salud de caché (0..100). Mejor con menos arquetipos
    // fragmentados y componentes < 1 línea de caché.
    float cache_health_score(const ProfileReport& rep) const {
        if (rep.archetypes.empty()) return 100.0f;
        float frag = static_cast<float>(count_sparse_archetypes()) / rep.archetypes.size();
        float avg_lines = 0.0f;
        for (const auto& c : rep.components) avg_lines += c.cache_lines_per_row;
        if (!rep.components.empty()) avg_lines /= rep.components.size();
        float score = (1.0f - frag) * 60.0f + (avg_lines <= 1.0f ? 40.0f : 40.0f / avg_lines);
        return score;
    }

private:
    const World& world_;
};

} // namespace dbg
} // namespace fluxdb
