// FluxDB — Feature #28: Cache & Fragmentation Profiler
// Ocupación de chunks, fragmentación de arquetipos, eficiencia de caché.
#include "../core/headers/ecs.h"
#include "../core/headers/profiler.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::dbg;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

struct Position { float x, y, z; };
struct Health { float hp; };

int main() {
    std::cout << "--- Starting FluxDB Cache & Fragmentation Profiler Test (#28) ---\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    ComponentID hp_id = store->register_component("Health", sizeof(Health));

    // 200 entidades con ambos componentes (un arquetipo denso).
    Position p{0,0,0};
    Health h{100.0f};
    for (int i = 0; i < 200; ++i) {
        Entity e = world.spawn();
        world.add_component(e, pos_id, &p);
        world.add_component(e, hp_id, &h);
    }

    CacheProfiler prof(world);
    ProfileReport rep = prof.profile();

    // 1. Totales (add_component secuencial crea arquetipos intermedios;
    // el denso Position+Health tiene las 200 entidades).
    CHECK(rep.archetype_count >= 1);
    CHECK(rep.total_entities == 200);
    CHECK(rep.components.size() == 2);

    // 2. Ocupación del arquetipo denso.
    bool found_dense = false;
    for (const auto& a : rep.archetypes) {
        if (a.entity_count == 200) {
            found_dense = true;
            CHECK(a.occupancy > 0.0f);
            CHECK(a.occupancy <= 1.0f);
        }
    }
    CHECK(found_dense);

    // 3. Eficiencia de caché por componente.
    for (const auto& c : rep.components) {
        CHECK(c.cache_lines_per_row > 0.0f);
        CHECK(c.efficiency > 0.0f && c.efficiency <= 1.0f);
        CHECK(c.bytes_total >= 200 * c.stride);
    }

    // 4. Bytes totales = suma de bytes de componentes.
    size_t expected_bytes = 0;
    for (const auto& c : rep.components) expected_bytes += c.bytes_total;
    CHECK(rep.total_bytes == expected_bytes);

    // 5. Sin arquetipos fragmentados con 200 entidades.
    CHECK(prof.count_sparse_archetypes() == 0);

    // 6. Health score en rango válido.
    float score = prof.cache_health_score(rep);
    CHECK(score >= 0.0f && score <= 100.0f);

    // 7. Fragmentación: crear un arquetipo con 2 entidades (sparse).
    Entity lone = world.spawn();
    world.add_component(lone, pos_id, &p); // solo Position → arquetipo distinto
    world.add_component(lone, pos_id, &p); // no-op re-add, no cambia estructura

    // Crear entidad con solo Health → 2do arquetipo sparse.
    Entity lone2 = world.spawn();
    world.add_component(lone2, hp_id, &h);

    CHECK(prof.count_sparse_archetypes() > 0);
    ProfileReport rep2 = prof.profile();
    CHECK(rep2.archetype_count >= 2);

    std::cout << "--- CACHE & FRAGMENTATION PROFILER TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}