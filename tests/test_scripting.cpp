#include "../core/headers/ecs.h"
#include "../core/headers/scripting.h"
#include <iostream>
#include <cassert>

using namespace veldradb;
using namespace veldradb::ecs;
using namespace veldradb::query;

struct PlayerStats {
    int gold;
};

int main() {
    std::cout << "--- Starting VeldraDB Lua Scripting Test ---\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, nullptr);
    
    ComponentID gold_id = store->register_component("gold", sizeof(int));
    
    Entity player = world.spawn();
    int initial_gold = 100;
    world.add_component(player, gold_id, &initial_gold);
    
    ScriptEngine engine(&world);
    
    // Test 1: Simple Lua Get
    std::string script1 = "local g = veldra.get(" + std::to_string(player) + ", 'gold')\n"
                          "print('Gold from Lua:', g)\n"
                          "assert(g == 100)";
    assert(engine.run_script(script1));
    std::cout << "1. Lua Get successful.\n";
    
    // Test 2: Lua Set (Logic inside database)
    std::string script2 = "local g = veldra.get(" + std::to_string(player) + ", 'gold')\n"
                          "veldra.set(" + std::to_string(player) + ", 'gold', g + 50)";
    assert(engine.run_script(script2));
    
    size_t size;
    const int* final_gold = (const int*)world.get_entity_component_data(player, gold_id, size);
    assert(*final_gold == 150);
    std::cout << "2. Lua Set (In-DB Logic) successful. New gold: " << *final_gold << "\n";
    
    std::cout << "--- SCRIPTING TEST PASSED ---\n";
    return 0;
}
