#include "../headers/spatial.h"
#include <cmath>
#include <algorithm>

namespace veldradb {
namespace spatial {

SpatialGrid::SpatialGrid(float cell_size) : cell_size_(cell_size) {
    if (cell_size_ <= 0.0f) {
        cell_size_ = 10.0f; // Default seguro
    }
}

SpatialGrid::Int3 SpatialGrid::get_cell_coords(const types::Vec3& pos) const {
    return {
        static_cast<int>(std::floor(pos.x_ / cell_size_)),
        static_cast<int>(std::floor(pos.y_ / cell_size_)),
        static_cast<int>(std::floor(pos.z_ / cell_size_))
    };
}

void SpatialGrid::insert(ecs::Entity entity, const types::Vec3& position) {
    Int3 cell = get_cell_coords(position);
    grid_[cell].push_back(entity);
}

void SpatialGrid::remove(ecs::Entity entity, const types::Vec3& position) {
    Int3 cell = get_cell_coords(position);
    auto it = grid_.find(cell);
    if (it != grid_.end()) {
        auto& ents = it->second;
        ents.erase(std::remove(ents.begin(), ents.end(), entity), ents.end());
        
        if (ents.empty()) {
            grid_.erase(it);
        }
    }
}

void SpatialGrid::update(ecs::Entity entity, const types::Vec3& old_pos, const types::Vec3& new_pos) {
    Int3 old_cell = get_cell_coords(old_pos);
    Int3 new_cell = get_cell_coords(new_pos);
    
    // Solo modificamos el hash si la entidad cruzó una frontera geométrica de celda
    if (old_cell == new_cell) {
        return;
    }
    
    remove(entity, old_pos);
    insert(entity, new_pos);
}

std::vector<ecs::Entity> SpatialGrid::find_near(const types::Vec3& origin, float radius) const {
    std::vector<ecs::Entity> results;
    
    // AABB Bounds to check
    types::Vec3 min_bound(origin.x_ - radius, origin.y_ - radius, origin.z_ - radius);
    types::Vec3 max_bound(origin.x_ + radius, origin.y_ + radius, origin.z_ + radius);

    Int3 min_cell = get_cell_coords(min_bound);
    Int3 max_cell = get_cell_coords(max_bound);
    
    // Search grid intersections
    for (int x = min_cell.x; x <= max_cell.x; ++x) {
        for (int y = min_cell.y; y <= max_cell.y; ++y) {
            for (int z = min_cell.z; z <= max_cell.z; ++z) {
                Int3 query_cell{x, y, z};
                auto it = grid_.find(query_cell);
                if (it != grid_.end()) {
                    // Solo añade entidades del grid en la celda broad-phase, no hace exact distance check.
                    // Generalmente VeldraDB filtra exact distance despues
                    for (auto ent : it->second) {
                        results.push_back(ent);
                    }
                }
            }
        }
    }

    return results;
}

} // namespace spatial
} // namespace veldradb
