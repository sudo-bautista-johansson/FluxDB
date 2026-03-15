#include "../headers/ecs.h"
#include <stdexcept>

namespace fluxdb {
namespace ecs {

ComponentID ComponentStore::register_component(const std::string& name, size_t size) {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) {
        return it->second;
    }

    if (components_.size() >= MAX_COMPONENTS) {
        throw std::runtime_error("Exceeded maximum number of component types");
    }

    ComponentID new_id = static_cast<ComponentID>(components_.size());
    components_.push_back({new_id, name, size});
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

} // namespace ecs
} // namespace fluxdb
