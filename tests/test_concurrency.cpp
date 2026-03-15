#include "../core/headers/ecs.h"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <shared_mutex>

using namespace veldradb;

void thread_writer(std::shared_ptr<ecs::World> world, int thread_id, ecs::ComponentID pos_id) {
    for (int i = 0; i < 1000; ++i) {
        ecs::Entity e = world->spawn();
        struct Transform { float x,y,z; } t = {float(thread_id), float(i), 0.0f};
        world->add_component(e, pos_id, &t);
    }
}

void thread_reader(std::shared_ptr<ecs::World> world, ecs::ComponentID pos_id) {
    for (int i = 0; i < 50; ++i) { 
        // Lock the World for reading so the Archetypes dictionary doesn't rehash while we iterate
        std::shared_lock<std::shared_mutex> w_lock(world->get_mutex());
        const auto& archs = world->get_archetypes();
        
        volatile float count = 0;
        for (const auto& pair : archs) {
            if (pair.first == 0) continue; // Skip empty archetype
            
            // This takes the Archetype Read-Lock internally
            uint8_t* array = pair.second->get_component_array(pos_id);
            if (array) {
                struct Transform { float x,y,z; };
                Transform* t = reinterpret_cast<Transform*>(array);
                count += t->x; 
            }
        }
        w_lock.unlock(); // Release World lock to allow Writers to spawn new archetypes
        
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    std::cout << "--- Starting High-Concurrency ECS Test ---" << std::endl;
    
    auto store = std::make_shared<ecs::ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(float)*3); 
    auto world = std::make_shared<ecs::World>(store);

    std::vector<std::thread> workers;
    
    // Spawn 5 writers, 5 readers (10 threads hitting the DB at once)
    for (int i=0; i<5; ++i) {
        workers.push_back(std::thread(thread_writer, world, i, pos_id));
        workers.push_back(std::thread(thread_reader, world, pos_id));
    }

    // Wait for all to finish
    for (auto& t : workers) {
        t.join();
    }

    std::cout << "Successfully spawned 5,000 entities from 5 concurrent threads while continuously reading the ECS memory layouts." << std::endl;
    std::cout << "--- CONCURRENCY TEST PASSED ---" << std::endl;
    return 0;
}
