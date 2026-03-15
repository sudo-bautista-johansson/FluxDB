#include <iostream>
#include <chrono>
#include <cassert>
#include "../core/headers/ecs.h"

using namespace veldradb::ecs;

struct Position {
    float x, y, z;
};

struct Health {
    float hp;
};

int main() {
    std::cout << "--- VeldraDB ECS Performance Test ---\n";
    
    auto store = std::make_shared<ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(Position));
    auto hp_id  = store->register_component("Health", sizeof(Health));

    World world(store);

    int NUM_ENTITIES = 1000000;
    std::cout << "Spawning " << NUM_ENTITIES << " entities...\n";

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_ENTITIES; i++) {
        Entity e = world.spawn();
        Position p{1.0f * i, 2.0f * i, 3.0f * i};
        Health h{100.0f};

        // All entities get Position
        world.add_component(e, pos_id, &p);

        // Half of them get Health
        if (i % 2 == 0) {
            world.add_component(e, hp_id, &h);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "✓ Spawned and populated all components in " << ms << " ms\n\n";

    std::cout << "--- Archetype Distribution ---\n";
    const auto& archetypes = world.get_archetypes();
    std::cout << "Total exact Archetypes created: " << archetypes.size() << "\n";
    
    for (const auto& [hash, arch] : archetypes) {
        std::cout << " - Archetype signature [" << hash << "] has " << arch->get_entity_count() << " contiguous entities.\n";
    }

    return 0;
}
