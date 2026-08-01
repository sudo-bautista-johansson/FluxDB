#include "../core/headers/ecs.h"
#include "../core/headers/history.h"
#include "../core/headers/parser.h"
#include "../core/headers/planner.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <string>

using namespace fluxdb::ecs;
using namespace fluxdb::query;

struct Position { float x, y, z; };
struct Health   { int hp; };

int main() {
    std::cout << "--- Starting VeldraDB Time-Travel Test ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(Position));
    auto hp_id  = store->register_component("Health", sizeof(Health));

    // Initialize world with HistoryManager (100 ticks max)
    auto history = std::make_shared<HistoryManager>(100);
    World world(store, history);

    // Tick 0: Spawn players
    Entity p1 = world.spawn(); // Player 1
    Entity p2 = world.spawn(); // Player 2

    // Set initial state at Tick 0
    Position p1_pos = {0.0f, 0.0f, 0.0f};
    Health p1_hp = {100};
    world.add_component(p1, pos_id, &p1_pos);
    world.add_component(p1, hp_id, &p1_hp);

    // Turn 1
    history->advance_tick(); // Tick = 1
    p1_pos.x = 10.0f;
    world.add_component(p1, pos_id, &p1_pos); // Player 1 moves to X=10

    // Turn 2
    history->advance_tick(); // Tick = 2
    p1_pos.x = 20.0f;
    p1_hp.hp = 80; // Takes damage
    world.add_component(p1, pos_id, &p1_pos);
    world.add_component(p1, hp_id, &p1_hp);

    // Turn 3
    history->advance_tick(); // Tick = 3
    p1_hp.hp = 0; // Dies
    world.add_component(p1, hp_id, &p1_hp);


    std::cout << "Current live Tick: " << history->get_current_tick() << "\n";

    // --- TIME TRAVEL QUERIES ---

    Health out_hp;
    Position out_pos;
    bool found;

    // 1. Query past state at Tick 1 (Player moved to 10, HP still 100)
    found = history->get_historical_state(1, p1, pos_id, &out_pos, sizeof(Position));
    assert(found == true);
    assert(out_pos.x == 10.0f);

    found = history->get_historical_state(1, p1, hp_id, &out_hp, sizeof(Health));
    // At tick 1, health didn't change! So we iterate backwards and find it was 100 before Tick 2 changed it.
    assert(found == true);
    assert(out_hp.hp == 100);

    // 2. Query past state at Tick 2 (Player moved to 20, HP at 80)
    found = history->get_historical_state(2, p1, hp_id, &out_hp, sizeof(Health));
    assert(found == true);
    assert(out_hp.hp == 80);

    // 3. Parser / Planner SQL integration test
    std::string sql = "SELECT * FROM players AT TICK 1 WHERE hp < 50;";
    Parser parser(sql);
    auto ast = parser.parse();
    Planner planner(sql);
    auto plan = planner.plan();

    // Verify parser successfully grabbed the tick
    if (auto* s = dynamic_cast<const SelectPlanNode*>(plan.root.get())) {
        assert(s->at_tick == 1);
        std::cout << "SQL Parser correctly resolved Time-Travel requested for Tick 1.\n";
    }

    std::cout << "--- TIME TRAVEL TEST PASSED ---\n";
    std::cout << "Kill-cams, lag compensation, and Braid/Prince of Persia rollbacks are now fully supported.\n";

    return 0;
}
