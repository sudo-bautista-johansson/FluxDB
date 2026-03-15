#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../core/headers/veldra_c_api.h"
#include "../core/headers/ecs.h"
#include "../core/headers/spatial_index.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <cstdio>

#include "../core/headers/veldra_internal.h"

int main() {
    std::cout << "--- Starting VeldraDB Spatial Index (Octree) Test ---\n";

    VeldraDB* db = (VeldraDB*)veldra_init();
    if (!db) {
        std::cerr << "Failed to init VeldraDB\n";
        return 1;
    }

    // Explicitly setup position component
    veldradb::ecs::ComponentID pos_id = db->world->get_store()->register_component("pos", 12);
    db->world->set_position_component_id(pos_id);
    
    std::cout << "1. Spawning 4 test entities...\n";
    float positions[][3] = {
        {0, 0, 0}, {5, 0, 0}, {10, 0, 0}, {100, 100, 100}
    };

    for (int i = 0; i < 4; ++i) {
        veldradb::ecs::Entity e = db->world->spawn();
        db->world->add_component(e, pos_id, positions[i]);
        std::cout << "   Entity " << e << " spawned at (" << positions[i][0] << ", " << positions[i][1] << ", " << positions[i][2] << ")\n";
    }

    std::cout << "2. Running SPATIAL_SCAN query via SQL...\n";
    const char* query = "FIND players NEAR (0, 0, 0) WITHIN 7";
    std::cout << "Executing: " << query << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    VeldraResult* res = veldra_query((struct VeldraDB*)db, query);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (!res) {
        std::cerr << "Query returned NULL\n";
        return 1;
    }
    
    std::cout << "--- QUERY RESULT ---\n";
    std::cout << veldra_result_get_text(res) << "\n";
    std::cout << "--------------------\n";
    
    std::chrono::duration<double, std::milli> elapsed = end - start;
    std::cout << "Time taken: " << elapsed.count() << "ms\n";

    veldra_free_result(res);
    veldra_close((struct VeldraDB*)db);

    std::cout << "--- SPATIAL INDEX TEST FINISHED ---\n";
    return 0;
}
