#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../core/headers/flux_c_api.h"
#include "../core/headers/flux_internal.h"
#include "../core/headers/ecs.h"
#include <iostream>
#include <cassert>
#include <chrono>

int main() {
    std::cout << "--- Starting FluxDB Spatial Index (Octree) Test ---\n";

    FluxDB* db = flux_init();
    if (!db) {
        std::cerr << "Failed to init FluxDB\n";
        return 1;
    }

    // Explicitly setup position component
    fluxdb::ecs::ComponentID pos_id = db->world->get_store()->register_component("pos", 12);
    db->world->set_position_component_id(pos_id);

    std::cout << "1. Spawning 4 test entities...\n";
    float positions[][3] = {
        {0, 0, 0}, {5, 0, 0}, {10, 0, 0}, {100, 100, 100}
    };

    for (int i = 0; i < 4; ++i) {
        fluxdb::ecs::Entity e = db->world->spawn();
        db->world->add_component(e, pos_id, positions[i]);
        std::cout << "   Entity " << e << " spawned at (" << positions[i][0] << ", " << positions[i][1] << ", " << positions[i][2] << ")\n";
    }

    std::cout << "2. Running SPATIAL_SCAN query via SQL...\n";
    const char* query = "FIND players NEAR (0, 0, 0) WITHIN 7";
    std::cout << "Executing: " << query << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    FluxResult* res = flux_query(db, query);
    auto end = std::chrono::high_resolution_clock::now();

    if (!res) {
        std::cerr << "Query returned NULL: " << flux_get_last_error() << "\n";
        return 1;
    }

    std::cout << "--- QUERY RESULT ---\n";
    std::cout << flux_result_get_text(res) << "\n";
    std::cout << "--------------------\n";

    std::chrono::duration<double, std::milli> elapsed = end - start;
    std::cout << "Time taken: " << elapsed.count() << "ms\n";

    flux_free_result(res);
    flux_close(db);

    std::cout << "--- SPATIAL INDEX TEST FINISHED ---\n";
    return 0;
}
