#pragma once

#include <vector>
#include <memory>
#include <unordered_map>
#include "common_types.h"

namespace fluxdb {
namespace spatial {

// A simple AABB for Octree bounds
struct Bounds {
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;

    bool contains(float x, float y, float z) const {
        return x >= min_x && x <= max_x &&
               y >= min_y && y <= max_y &&
               z >= min_z && z <= max_z;
    }

    bool intersects(const Bounds& other) const {
        return (min_x <= other.max_x && max_x >= other.min_x) &&
               (min_y <= other.max_y && max_y >= other.min_y) &&
               (min_z <= other.max_z && max_z >= other.min_z);
    }
};

struct SpatialObject {
    veldradb::ecs::Entity entity;
    float x, y, z;
};

class OctreeNode {
public:
    OctreeNode(const Bounds& bounds, int depth = 0);
    ~OctreeNode();

    void insert(SpatialObject obj);
    void remove(veldradb::ecs::Entity entity, float x, float y, float z);
    void query(const Bounds& query_bounds, std::vector<veldradb::ecs::Entity>& results);

private:
    void subdivide();
    int get_child_index(float x, float y, float z);

    Bounds bounds_;
    int depth_;
    static constexpr int MAX_DEPTH = 8;
    static constexpr int MAX_OBJECTS = 16;

    std::vector<SpatialObject> objects_;
    std::unique_ptr<OctreeNode> children_[8];
    bool is_leaf_ = true;
};

class SpatialIndex {
public:
    SpatialIndex(Bounds world_bounds);
    
    void update_entity(veldradb::ecs::Entity entity, float x, float y, float z);
    void remove_entity(veldradb::ecs::Entity entity);
    
    void query_range(float x, float y, float z, float radius, std::vector<veldradb::ecs::Entity>& results);

private:
    Bounds world_bounds_;
    OctreeNode root_;
    
    struct EntityInfo {
        float x, y, z;
    };
    std::unordered_map<veldradb::ecs::Entity, EntityInfo> entities_;
};

} // namespace spatial
} // namespace fluxdb
