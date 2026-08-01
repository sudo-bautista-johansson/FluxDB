// FluxDB — Feature #29: Visual Query Plan Explainer
// Dump introspectivo del plan compilado: arquetipos, entidades, coste,
// y detección de patrones lentos.
#include "../core/headers/ecs.h"
#include "../core/headers/explainer.h"
#include <iostream>
#include <cassert>
#include <sstream>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::dbg;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

struct Position { float x, y, z; };
struct Health { float hp; };
struct Velocity { float vx, vy, vz; };

static std::string render(const ExplainResult& r) {
    std::stringstream ss;
    for (const auto& line : r.lines) ss << line.text << "\n";
    return ss.str();
}

int main() {
    std::cout << "--- Starting FluxDB Query Plan Explainer Test (#29) ---\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    ComponentID hp_id = store->register_component("Health", sizeof(Health));
    ComponentID vel_id = store->register_component("Velocity", sizeof(Velocity));

    // 100 entidades con Position+Health.
    Position p{0,0,0};
    Health h{100.0f};
    for (int i = 0; i < 100; ++i) {
        Entity e = world.spawn();
        world.add_component(e, pos_id, &p);
        world.add_component(e, hp_id, &h);
    }
    // 3 entidades con Velocity (arquetipo disperso).
    Velocity v{0,0,0};
    for (int i = 0; i < 3; ++i) {
        Entity e = world.spawn();
        world.add_component(e, vel_id, &v);
    }

    QueryExplainer exp(world);

    // 1. Query densa: Position+Health.
    QueryHandle q1 = world.create_query({pos_id, hp_id});
    ExplainResult r1 = exp.explain(q1);
    CHECK(r1.components.size() == 2);
    CHECK(r1.estimated_entities == 100);
    CHECK(r1.matched_archetypes >= 1);
    CHECK(r1.lines.size() >= 3);
    CHECK(!r1.slow_pattern);

    // El explain debe mencionar el componente y el conteo.
    std::string out1 = render(r1);
    CHECK(out1.find("components") != std::string::npos);
    CHECK(out1.find("entities=100") != std::string::npos);

    // 2. Query que matchea el arquetipo disperso (Position+Health+Velo: 0).
    QueryHandle q2 = world.create_query({pos_id, hp_id, vel_id});
    ExplainResult r2 = exp.explain(q2);
    CHECK(r2.estimated_entities == 0);
    CHECK(r2.slow_pattern); // dead query
    std::string out2 = render(r2);
    CHECK(out2.find("dead query") != std::string::npos);

    // 3. Handle inválido.
    ExplainResult r3 = exp.explain(9999);
    CHECK(r3.slow_pattern);
    CHECK(render(r3).find("INVALID") != std::string::npos);

    // 4. Query con solo Velocity → arquetipo sparse flagado.
    QueryHandle q4 = world.create_query({vel_id});
    ExplainResult r4 = exp.explain(q4);
    CHECK(r4.estimated_entities == 3);

    std::cout << "--- QUERY PLAN EXPLAINER TEST PASSED (" << checks << " checks) ---\n";
    std::cout << render(r1);
    return 0;
}