#include "../core/headers/ecs.h"
#include "../core/headers/network.h"
#include "../core/headers/delta_set.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::network;

struct Health {
    int hp;
};

struct Ammo {
    int count;
};

static uint32_t read_u32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

int main() {
    std::cout << "--- Starting FluxDB Network Delta Compression Test ---\n";
    
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
    
    assert(read_u32(payload, 0) == delta::DeltaSet::kMagic);
    assert(payload[4] == delta::DeltaSet::kVersion); // version (v2: + RELATION events)
    assert(read_u32(payload, 21) == 0); // num_codecs
    assert(read_u32(payload, 25) == 0); // num_records
    assert(payload.size() == 29); // header only
    
    std::cout << "1. Base state generated header-only payload as expected.\n";
    
    world.get_history()->advance_tick(); // Tick + 1
    h1.hp = 80;
    world.add_component(player1, health_id, &h1);
    
    world.get_history()->advance_tick(); // Tick + 2
    Ammo a2{50};
    world.add_component(player2, ammo_id, &a2);
    
    delta.generate_delta_payload(initial_tick, payload);
    
    assert(read_u32(payload, 25) == 2);
    // Header(29) + 2 * (Op(1) + Entity(4) + CompID(1) + Size(4) + OrigSize(4) + Payload(4)) = 65
    assert(payload.size() == 65);
    
    std::cout << "2. Delta payload generated correctly (" << payload.size() << " bytes) for 2 dirty components.\n";
    
    // El payload ES un DeltaSet unificado: se puede deserializar y aplicar.
    delta::DeltaSet set;
    assert(set.deserialize(world.codec_registry(), payload.data(), payload.size()));
    assert(set.record_count() == 2);
    assert(set.base_tick() == initial_tick);
    assert(set.end_tick() == world.current_tick());
    
    int updates_found = 0;
    set.for_each_record([&](const delta::DeltaRecord& r) {
        assert(r.op == delta::DeltaOp::UPDATE);
        assert(r.data.size() == sizeof(Health));
        ++updates_found;
    });
    assert(updates_found == 2);
    
    std::cout << "3. Payload round-trips through DeltaSet deserialize (2 update records).\n";
    
    std::cout << "--- DELTA COMPRESSION TEST PASSED ---\n";
    std::cout << "UDP Network Deltas are fully working (Unified Delta Engine format).\n";
    return 0;
}
