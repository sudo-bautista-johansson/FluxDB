#pragma once

#include "types.h"
#include "ecs.h"
#include <vector>
#include <unordered_map>
#include <functional>

namespace veldradb {
namespace spatial {

// ─────────────────────────────────────────
//  Spatial Hash Grid 3D
// ─────────────────────────────────────────
// Optimal for quickly finding entities NEAR a point within a uniform radius.

class SpatialGrid {
public:
    explicit SpatialGrid(float cell_size);

    // Mapea una entidad a una celda física basada en su posición
    void insert(ecs::Entity entity, const types::Vec3& position);

    // Actualiza la posición de una entidad (la mueve de celda si es necesario)
    void update(ecs::Entity entity, const types::Vec3& old_pos, const types::Vec3& new_pos);

    // Quita la entidad del grid (despawn o ya no tiene componente espacial)
    void remove(ecs::Entity entity, const types::Vec3& position);

    // Devuelve lista de entidades en las celdas directamente afectadas por un radio
    std::vector<ecs::Entity> find_near(const types::Vec3& origin, float radius) const;

private:
    // Hash robusto diseñado para coordenadas 3D y juegos
    struct Int3 {
        int x, y, z;
        bool operator==(const Int3& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct Int3Hash {
        std::size_t operator()(const Int3& key) const {
            // Constantes primas grandes para hash
            return (key.x * 73856093) ^ (key.y * 19349663) ^ (key.z * 83492791);
        }
    };

    Int3 get_cell_coords(const types::Vec3& pos) const;

    float cell_size_;
    std::unordered_map<Int3, std::vector<ecs::Entity>, Int3Hash> grid_;
};

} // namespace spatial
} // namespace veldradb
