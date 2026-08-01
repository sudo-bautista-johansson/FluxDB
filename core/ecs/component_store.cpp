#include "../headers/ecs.h"
#include <stdexcept>

namespace fluxdb {
namespace ecs {

ComponentID ComponentStore::register_component(const std::string& name, size_t size) {
    return register_component(name, size, ComponentTier::WARM);
}

ComponentID ComponentStore::register_component(const std::string& name, size_t size, ComponentTier tier) {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) {
        return it->second;
    }

    if (components_.size() >= MAX_COMPONENTS) {
        throw std::runtime_error("Exceeded maximum number of component types");
    }

    ComponentID new_id = static_cast<ComponentID>(components_.size());
    components_.push_back({new_id, name, size, tier});
    name_to_id_[name] = new_id;
    return new_id;
}

ComponentID ComponentStore::get_id(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        throw std::runtime_error("Component not found: " + name);
    }
    return it->second;
}

const ComponentInfo& ComponentStore::get_info(ComponentID id) const {
    if (id >= components_.size()) {
        throw std::runtime_error("Invalid component ID");
    }
    return components_[id];
}

// ── Hot/Cold Archetype Splitting (#1) ──

void ComponentStore::set_tier(ComponentID id, ComponentTier tier) {
    if (id >= components_.size()) {
        throw std::runtime_error("Invalid component ID");
    }
    components_[id].tier = tier;
}

ComponentTier ComponentStore::get_tier(ComponentID id) const {
    if (id >= components_.size()) {
        throw std::runtime_error("Invalid component ID");
    }
    return components_[id].tier;
}

ComponentTier ComponentStore::get_tier(const std::string& name) const {
    return get_tier(get_id(name));
}

} // namespace ecs
} // namespace fluxdb
