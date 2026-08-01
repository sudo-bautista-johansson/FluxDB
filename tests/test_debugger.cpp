// FluxDB — Feature #27: Time-Travel World Debugger
// Scrub del timeline, diffs entre ticks, atribución de escrituras.
#include "../core/headers/ecs.h"
#include "../core/headers/debugger.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::dbg;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

struct Health { float hp; };

int main() {
    std::cout << "--- Starting FluxDB Time-Travel Debugger Test (#27) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(64);
    World world(store, history);

    ComponentID hp_id = store->register_component("Health", sizeof(Health));

    Entity e1 = world.spawn();
    Health h{100.0f};            // tick 0: 100
    world.add_component(e1, hp_id, &h);
    world.advance_tick();        // tick 1

    h.hp = 80.0f;                // cambio en tick 1 (se guarda old=100)
    world.add_component(e1, hp_id, &h);
    world.advance_tick();        // tick 2

    h.hp = 40.0f;                // cambio en tick 2 (se guarda old=80)
    world.add_component(e1, hp_id, &h);
    world.advance_tick();        // tick 3 (live = 40)

    TimeTravelDebugger dbg(world);
    CHECK(dbg.now() == 3);

    // 1. Estado histórico exacto en cada tick.
    DebuggedValue v0 = dbg.state_at(0, e1, hp_id);
    CHECK(v0.found);
    float hp0; std::memcpy(&hp0, v0.bytes.data(), sizeof(float));
    CHECK(std::fabs(hp0 - 100.0f) < 1e-5f);

    DebuggedValue v1 = dbg.state_at(1, e1, hp_id);
    float hp1; std::memcpy(&hp1, v1.bytes.data(), sizeof(float));
    CHECK(std::fabs(hp1 - 80.0f) < 1e-5f);

    DebuggedValue v2 = dbg.state_at(2, e1, hp_id);
    float hp2; std::memcpy(&hp2, v2.bytes.data(), sizeof(float));
    CHECK(std::fabs(hp2 - 40.0f) < 1e-5f);

    // 2. changed_between detecta el cambio 0→2, no 2→3.
    CHECK(dbg.changed_between(e1, hp_id, 0, 2));
    CHECK(!dbg.changed_between(e1, hp_id, 2, 3));

    // 3. scrub del timeline (4 ticks: 0..3).
    auto frames = dbg.scrub(e1, hp_id, 0, 3);
    CHECK(frames.size() == 4);
    CHECK(frames[0].bytes == v0.bytes);
    CHECK(frames[3].bytes == v2.bytes);

    // 4. Atribución de escritura (último write en tick 2).
    DebugWrite w = dbg.last_write(e1, hp_id);
    CHECK(w.last_write_tick == 2);

    // 5. Diff desde tick 1: e1+Health está en los cambios (cambio en tick 2).
    auto changes = dbg.diff(1, 3);
    bool found_e1 = false;
    for (const auto& c : changes) {
        if (c.entity == e1 && c.comp_id == hp_id) found_e1 = true;
    }
    CHECK(found_e1);

    // 6. Divergencia entre dos mundos (simula client/server desync).
    auto storeB = std::make_shared<ComponentStore>();
    auto historyB = std::make_shared<HistoryManager>(64);
    World worldB(storeB, historyB);
    ComponentID hp_idB = storeB->register_component("Health", sizeof(Health));

    Entity eB = worldB.spawn();
    Health hb{100.0f};
    worldB.add_component(eB, hp_idB, &hb);
    worldB.advance_tick();
    hb.hp = 80.0f;
    worldB.add_component(eB, hp_idB, &hb);
    worldB.advance_tick();
    hb.hp = 55.0f; // DESYNC: el servidor tiene 40
    worldB.add_component(eB, hp_idB, &hb);
    worldB.advance_tick();

    TimeTravelDebugger dbgB(worldB);
    CHECK(dbg.first_divergence(e1, hp_id, worldB, 0, 3) == 2);

    std::cout << "--- TIME-TRAVEL DEBUGGER TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}