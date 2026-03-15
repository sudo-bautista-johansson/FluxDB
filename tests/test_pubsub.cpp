#include "../core/headers/ecs.h"
#include "../core/headers/pubsub.h"
#include "../core/headers/parser.h"
#include <iostream>
#include <cassert>

using namespace veldradb;
using namespace veldradb::ecs;
using namespace veldradb::query;

struct Position {
    float x, y, z;
};

int main() {
    std::cout << "--- Starting VeldraDB Spatial Pub/Sub Test ---\n";
    
    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    
    World world(store, nullptr, pubsub);
    
    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    world.set_position_component_id(pos_id);
    
    Entity player1 = world.spawn();
    
    bool p1_entered = false;
    bool p1_exited = false;
    
    // Subscribe to area: Center(10, 0, 0) Radius 5
    uint32_t sub_id = pubsub->subscribe_spatial(
        "", 10.0f, 0.0f, 0.0f, 5.0f, 
        [&](uint32_t e, bool entered) {
            std::cout << "Event received for entity " << e << ": " << (entered ? "ENTERED" : "EXITED") << "\n";
            if (e == player1) {
                if (entered) p1_entered = true;
                else p1_exited = true;
            }
        });
        
    std::cout << "1. Move player outside zone (0,0,0) - No callback\n";
    Position p;
    p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
    world.add_component(player1, pos_id, &p); 
    
    assert(!p1_entered);
    assert(!p1_exited);
    
    std::cout << "2. Move player inside zone (8,0,0) - Should trigger entered\n";
    p.x = 8.0f;
    world.add_component(player1, pos_id, &p); 
    
    assert(p1_entered);
    assert(!p1_exited);
    
    p1_entered = false; // reset flag
    
    std::cout << "3. Move player inside zone (9,0,0) - Should not trigger\n";
    p.x = 9.0f;
    world.add_component(player1, pos_id, &p);
    
    assert(!p1_entered);
    assert(!p1_exited);
    
    std::cout << "4. Move player outside zone (20,0,0) - Should trigger exited\n";
    p.x = 20.0f;
    world.add_component(player1, pos_id, &p);
    
    assert(!p1_entered);
    assert(p1_exited);
    
    // Also test Parser for LISTEN SELECT
    Parser parser("LISTEN SELECT * FROM players WHERE distance(pos, [10, 0, 0]) < 5;");
    auto ast = parser.parse();
    auto* select_stmt = dynamic_cast<SelectStatement*>(ast.get());
    assert(select_stmt != nullptr);
    assert(select_stmt->is_listen == true);
    
    std::cout << "--- PUBSUB TEST PASSED ---\n";
    std::cout << "Reactive Spatial Queries are fully working.\n";
    return 0;
}
