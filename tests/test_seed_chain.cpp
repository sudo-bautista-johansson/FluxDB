// FluxDB — Feature #21: Deterministic Seed-Chain Components
// Semillas jerárquicas deterministas por entidad, sin estado global,
// para procedural generation reproducible en lockstep y red.
#include "../core/headers/ecs.h"
#include "../core/headers/seed_chain.h"
#include "../core/headers/fixed.h"
#include "../core/headers/delta_codec.h"
#include "../core/headers/delta_set.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb::ecs;
using namespace fluxdb::det;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_seed_chain_struct() {
    std::cout << "1. SeedChain struct y derivación jerárquica...\n";

    // Semilla base a partir de sector + índice local
    SeedChain s;
    s.world_seed = 12345;
    s.sector_x = 100; s.sector_y = 200; s.sector_z = 0;
    s.local_index = 0;

    uint64_t base = s.chain_seed();
    CHECK(base != 0);
    CHECK(base != s.world_seed); // mezcló bien

    // Misma entrada → misma salida (determinismo)
    SeedChain s2 = s;
    CHECK(s.chain_seed() == s2.chain_seed());

    // Índice local diferente → semilla diferente
    SeedChain s3 = s;
    s3.local_index = 1;
    CHECK(s.chain_seed() != s3.chain_seed());

    // child_seed derivado
    uint64_t child0 = s.child_seed(0);
    uint64_t child1 = s.child_seed(1);
    CHECK(child0 != child1);
    CHECK(child0 != s.chain_seed());

    // RNG a partir de la seed: secuencia determinista
    FixedRandom rng(s.chain_seed());
    FixedRandom rng2(s.chain_seed());
    for (int i = 0; i < 20; ++i) {
        CHECK(rng.next_u64() == rng2.next_u64());
    }
}

static void test_seed_ecs_component() {
    std::cout << "2. SeedChain como componente ECS...\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(600);
    World world(store, history);

    ComponentID seed_id = store->register_component("SeedChain", sizeof(SeedChain));
    CHECK(seed_id < 255);

    Entity e = world.spawn();
    SeedChain s;
    s.world_seed = 42;
    s.sector_x = 5; s.sector_y = -3; s.sector_z = 0;
    s.local_index = 7;
    world.add_component(e, seed_id, &s);

    size_t sz = 0;
    const SeedChain* got = static_cast<const SeedChain*>(world.get_entity_component_data(e, seed_id, sz));
    CHECK(got != nullptr);
    CHECK(sz == sizeof(SeedChain));
    CHECK(got->chain_seed() == s.chain_seed());
    CHECK(got->sector_x == 5);
}

static void test_seed_deterministic_regen() {
    std::cout << "3. Generación procedural determinista desde SeedChain...\n";

    // Dos worlds independientes con la misma seed producen los mismos
    // valores procedurales (lockstep: #11).
    auto make_world = [](uint64_t seed) {
        auto store = std::make_shared<ComponentStore>();
        auto history = std::make_shared<HistoryManager>(600);
        World world(store, history);
        ComponentID sid = store->register_component("Seed", sizeof(SeedChain));

        Entity e = world.spawn();
        SeedChain sc;
        sc.world_seed = seed;
        sc.sector_x = 1; sc.sector_y = 2; sc.sector_z = 3;
        sc.local_index = 42;
        world.add_component(e, sid, &sc);

        // Leer la seed y generar valores procedurales
        size_t sz = 0;
        const SeedChain* sp = static_cast<const SeedChain*>(world.get_entity_component_data(e, sid, sz));
        FixedRandom rng(sp->chain_seed());
        float vals[10];
        for (int i = 0; i < 10; ++i) {
            vals[i] = rng.next_fix01().to_float();
        }
        return std::make_tuple(vals[0], vals[1], vals[2], vals[3], vals[4],
                               vals[5], vals[6], vals[7], vals[8], vals[9]);
    };

    auto [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9] = make_world(9999);
    auto [b0, b1, b2, b3, b4, b5, b6, b7, b8, b9] = make_world(9999);

    CHECK(a0 == b0); CHECK(a1 == b1); CHECK(a2 == b2);
    CHECK(a3 == b3); CHECK(a4 == b4); CHECK(a5 == b5);
    CHECK(a6 == b6); CHECK(a7 == b7); CHECK(a8 == b8);
    CHECK(a9 == b9);

    // Semilla diferente → valores diferentes
    auto [c0, c1, _, _3, _4, _5, _6, _7, _8, _9] = make_world(1234);
    CHECK(a0 != c0); // casi seguro con alta probabilidad
}

int main() {
    std::cout << "--- Starting FluxDB Seed-Chain Components Test (#21) ---\n";
    test_seed_chain_struct();
    test_seed_ecs_component();
    test_seed_deterministic_regen();
    std::cout << "--- SEED CHAIN TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}