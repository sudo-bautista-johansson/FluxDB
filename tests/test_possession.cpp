// FluxDB — Feature #31: Live Server-Authoritative Entity Possession
// Sesión de edición en vivo: posesión, edits versionados, audit trail, undo.
#include "../core/headers/ecs.h"
#include "../core/headers/possession.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::rollback;
using namespace fluxdb::dbg;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

struct SpawnRate { float rate; };

int main() {
    std::cout << "--- Starting FluxDB Live Possession Test (#31) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(64);
    World world(store, history);

    // Ring buffer de rollback (#8) para undo.
    auto ring = std::make_shared<SnapshotRingBuffer>(16);
    world.attach_rollback(ring.get());

    ComponentID spawn_id = store->register_component("SpawnRate", sizeof(SpawnRate));

    // Mundo con dos entidades.
    Entity e1 = world.spawn();
    Entity e2 = world.spawn();
    SpawnRate s{1.0f};
    world.add_component(e1, spawn_id, &s);
    world.add_component(e2, spawn_id, &s);
    world.advance_tick(); // tick 1
    ring->capture(world); // base: estado original (rate 1.0)

    PossessionSession session(world);

    // 1. Posesión.
    CHECK(session.possess(e1));
    CHECK(session.possess(e2));
    CHECK(session.possessed_count() == 2);
    CHECK(session.is_possessed(e1));
    CHECK(!session.possess(99999)); // no existe

    // 2. Edición versionada (balance tweak) con audit trail.
    SpawnRate tweak{2.5f};
    uint64_t seq1 = session.edit(e1, spawn_id, &tweak, sizeof(SpawnRate));
    CHECK(seq1 == 1);
    CHECK(session.edit_count() == 1);

    size_t sz = 0;
    const SpawnRate* cur = static_cast<const SpawnRate*>(
        world.get_entity_component_data(e1, spawn_id, sz));
    CHECK(cur != nullptr);
    CHECK(std::fabs(cur->rate - 2.5f) < 1e-5f);

    PossessionEdit le = session.last_edit();
    CHECK(le.entity == e1);
    CHECK(le.comp_id == spawn_id);
    CHECK(le.seq == 1);

    // 3. Las ediciones de entidades NO poseídas se rechazan.
    Entity e3 = world.spawn();
    CHECK(session.edit(e3, spawn_id, &tweak, sizeof(SpawnRate)) == 0);

    // 4. Undo revierte al estado del ring (rate 1.0, tick 1).
    CHECK(session.undo());
    CHECK(session.edit_count() == 0);

    sz = 0;
    const SpawnRate* reverted = static_cast<const SpawnRate*>(
        world.get_entity_component_data(e1, spawn_id, sz));
    CHECK(reverted != nullptr);
    CHECK(std::fabs(reverted->rate - 1.0f) < 1e-5f); // valor original

    // 5. Undo sin ediciones → false.
    CHECK(!session.undo());

    std::cout << "--- LIVE POSSESSION TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}