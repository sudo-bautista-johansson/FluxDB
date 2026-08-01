#include "../headers/spatial_index.h"
#include <cmath>
#include <algorithm>

namespace fluxdb {
namespace spatial {

OctreeNode::OctreeNode(const Bounds& bounds, int depth) : bounds_(bounds), depth_(depth) {
    for (int i = 0; i < 8; ++i) children_[i] = nullptr;
}

OctreeNode::~OctreeNode() {}

void OctreeNode::subdivide() {
    float mid_x = (bounds_.min_x + bounds_.max_x) * 0.5f;
    float mid_y = (bounds_.min_y + bounds_.max_y) * 0.5f;
    float mid_z = (bounds_.min_z + bounds_.max_z) * 0.5f;

    children_[0] = std::make_unique<OctreeNode>(Bounds{bounds_.min_x, bounds_.min_y, bounds_.min_z, mid_x, mid_y, mid_z}, depth_ + 1);
    children_[1] = std::make_unique<OctreeNode>(Bounds{mid_x, bounds_.min_y, bounds_.min_z, bounds_.max_x, mid_y, mid_z}, depth_ + 1);
    children_[2] = std::make_unique<OctreeNode>(Bounds{bounds_.min_x, mid_y, bounds_.min_z, mid_x, bounds_.max_y, mid_z}, depth_ + 1);
    children_[3] = std::make_unique<OctreeNode>(Bounds{mid_x, mid_y, bounds_.min_z, bounds_.max_x, bounds_.max_y, mid_z}, depth_ + 1);
    children_[4] = std::make_unique<OctreeNode>(Bounds{bounds_.min_x, bounds_.min_y, mid_z, mid_x, mid_y, bounds_.max_z}, depth_ + 1);
    children_[5] = std::make_unique<OctreeNode>(Bounds{mid_x, bounds_.min_y, mid_z, bounds_.max_x, mid_y, bounds_.max_z}, depth_ + 1);
    children_[6] = std::make_unique<OctreeNode>(Bounds{bounds_.min_x, mid_y, mid_z, mid_x, bounds_.max_y, bounds_.max_z}, depth_ + 1);
    children_[7] = std::make_unique<OctreeNode>(Bounds{mid_x, mid_y, mid_z, bounds_.max_x, bounds_.max_y, bounds_.max_z}, depth_ + 1);

    is_leaf_ = false;
    for (auto& obj : objects_) {
        int idx = get_child_index(obj.x, obj.y, obj.z);
        children_[idx]->insert(obj);
    }
    objects_.clear();
}

int OctreeNode::get_child_index(float x, float y, float z) {
    float mid_x = (bounds_.min_x + bounds_.max_x) * 0.5f;
    float mid_y = (bounds_.min_y + bounds_.max_y) * 0.5f;
    float mid_z = (bounds_.min_z + bounds_.max_z) * 0.5f;

    int idx = 0;
    if (x >= mid_x) idx |= 1;
    if (y >= mid_y) idx |= 2;
    if (z >= mid_z) idx |= 4;
    return idx;
}

void OctreeNode::insert(SpatialObject obj) {
    if (!is_leaf_) {
        int idx = get_child_index(obj.x, obj.y, obj.z);
        children_[idx]->insert(obj);
        return;
    }

    objects_.push_back(obj);

    if (objects_.size() > MAX_OBJECTS && depth_ < MAX_DEPTH) {
        subdivide();
    }
}

void OctreeNode::remove(Entity entity, float x, float y, float z) {
    if (!is_leaf_) {
        int idx = get_child_index(x, y, z);
        children_[idx]->remove(entity, x, y, z);
        return;
    }

    auto it = std::remove_if(objects_.begin(), objects_.end(), [entity](const SpatialObject& o) {
        return o.entity == entity;
    });
    objects_.erase(it, objects_.end());
}

void OctreeNode::query(const Bounds& query_bounds, std::vector<Entity>& results) {
    if (!bounds_.intersects(query_bounds)) return;

    if (!is_leaf_) {
        for (int i = 0; i < 8; ++i) {
            children_[i]->query(query_bounds, results);
        }
        return;
    }

    for (const auto& obj : objects_) {
        if (query_bounds.contains(obj.x, obj.y, obj.z)) {
            results.push_back(obj.entity);
        }
    }
}

SpatialIndex::SpatialIndex(Bounds world_bounds) : world_bounds_(world_bounds), root_(world_bounds) {}

void SpatialIndex::update_entity(Entity entity, float x, float y, float z) {
    auto it = entities_.find(entity);
    if (it != entities_.end()) {
        root_.remove(entity, it->second.x, it->second.y, it->second.z);
    }
    
    entities_[entity] = {x, y, z};
    root_.insert({entity, x, y, z});
}

void SpatialIndex::remove_entity(Entity entity) {
    auto it = entities_.find(entity);
    if (it != entities_.end()) {
        root_.remove(entity, it->second.x, it->second.y, it->second.z);
        entities_.erase(it);
    }
}

void SpatialIndex::query_range(float x, float y, float z, float radius, std::vector<Entity>& results) {
    Bounds query_bounds{x - radius, y - radius, z - radius, x + radius, y + radius, z + radius};
    root_.query(query_bounds, results);
}

} // namespace spatial
} // namespace fluxdb
