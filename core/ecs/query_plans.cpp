#include "../headers/query_plans.h"
#include <algorithm>
#include <mutex>

namespace fluxdb {
namespace ecs {

QueryPlan::QueryPlan(std::vector<ComponentID> required, const ComponentStore& store)
    : components_(std::move(required)) {
    for (ComponentID c : components_) {
        signature_.set(c);
    }
}

void QueryPlan::add_archetype(Archetype* arch, const ComponentStore& store) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    for (const MatchedArchetype& existing : matches_) {
        if (existing.archetype == arch) {
            return; // ya está en el plan
        }
    }

    MatchedArchetype m;
    m.archetype = arch;
    for (ComponentID c : components_) {
        QueryAccessor acc;
        acc.comp_id = c;
        acc.stride = static_cast<uint32_t>(store.get_info(c).size);
        m.accessors.push_back(acc);
    }
    matches_.push_back(std::move(m));
    ++version_;
}

void QueryPlan::remove_archetype(Archetype* arch) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = std::find_if(matches_.begin(), matches_.end(),
                           [arch](const MatchedArchetype& m) { return m.archetype == arch; });
    if (it == matches_.end()) {
        return; // el plan no matcheaba ese arquetipo
    }

    matches_.erase(it);
    ++version_;
}

} // namespace ecs
} // namespace fluxdb
