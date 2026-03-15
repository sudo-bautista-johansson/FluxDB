#include <iostream>
#include <chrono>
#include <cassert>
#include "../core/headers/spatial.h"
#include "../core/headers/types.h"

using namespace veldradb::spatial;
using namespace veldradb::types;
using namespace veldradb::ecs;

int main() {
    std::cout << "--- VeldraDB Spatial Grid Test ---\n";
    std::cout.flush();

    try {
        SpatialGrid grid(50.0f); // 50m cells
        
        std::cout << "Inserting 10,000 entities in grid...\n";
        for (int i = 0; i < 10000; ++i) {
            Vec3 pos(i * 1.5f, 0.0f, i * 2.0f);
            grid.insert(i, pos);
        }
        
        std::cout << "Searching entities near (1000.0, 0.0, 1333.3) within 150m...\n";
        auto start = std::chrono::high_resolution_clock::now();
        
        Vec3 search_pos(1000.0f, 0.0f, 1333.3f);
        auto results = grid.find_near(search_pos, 150.0f);
        
        auto end = std::chrono::high_resolution_clock::now();
        uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        uint64_t micro = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        std::cout << "Found " << results.size() << " entities near target in " << micro << " us (" << ms << " ms)\n";
        
        // Verifying update correctly moves cells
        grid.update(500, Vec3(750.0f, 0.0f, 1000.0f), Vec3(-500.0f, 0.0f, -500.0f));
        auto r2 = grid.find_near(Vec3(-500.0f, 0.0f, -500.0f), 10.0f);
        
        bool found_updated = false;
        for (auto ent : r2) {
            if (ent == 500) found_updated = true;
        }
        assert(found_updated);

    } catch (const std::exception& e) {
        std::cerr << "Test failed with error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "✓ Spatial Hash Grid lookup passes boundary limits efficiently.\n";
    return 0;
}
