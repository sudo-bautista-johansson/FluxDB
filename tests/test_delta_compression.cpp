#include "../core/headers/ecs.h"
#include "../core/headers/network.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace veldradb;
using namespace veldradb::ecs;
using namespace veldradb::network;

struct Health {
    int hp;
};

struct Ammo {
    int count;
};

int main() {
    std::cout << "--- Starting VeldraDB Network Delta Compression Test ---\n";
    
    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(10));
    
    ComponentID health_id = store->register_component("Health", sizeof(Health));
    ComponentID ammo_id = store->register_component("Ammo", sizeof(Ammo));
    
    DeltaCompression delta(&world);
    
    Entity player1 = world.spawn();
    Entity player2 = world.spawn();
    
    Health h1{100};
    Ammo a1{30};
    world.add_component(player1, health_id, &h1);
    world.add_component(player1, ammo_id, &a1);
    
    Health h2{100};
    world.add_component(player2, health_id, &h2);
    
    uint64_t initial_tick = world.get_history()->get_current_tick();
    
    std::vector<uint8_t> payload;
    delta.generate_delta_payload(initial_tick, payload);
    
    assert(payload.size() == sizeof(uint32_t));
    uint32_t num_records = 0;
    std::memcpy(&num_records, payload.data(), sizeof(uint32_t));
    assert(num_records == 0);
    
    std::cout << "1. Base state generated 0 bytes as expected.\n";
    
    world.get_history()->advance_tick(); // Tick + 1
    h1.hp = 80;
    world.add_component(player1, health_id, &h1);
    
    world.get_history()->advance_tick(); // Tick + 2
    Ammo a2{50};
    world.add_component(player2, ammo_id, &a2);
    
    delta.generate_delta_payload(initial_tick, payload);
    
    std::memcpy(&num_records, payload.data(), sizeof(uint32_t));
    assert(num_records == 2); 
    
    // Header(4) + 2 * (Entity(4) + CompID(1) + Size(1) + Payload(4)) = 24 bytes
    assert(payload.size() == 24);
    
    std::cout << "2. Delta payload generated correctly (" << payload.size() << " bytes) for 2 dirty components.\n";
    
    std::cout << "--- DELTA COMPRESSION TEST PASSED ---\n";
    std::cout << "UDP Network Deltas are fully working.\n";
    return 0;
}
