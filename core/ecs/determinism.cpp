#include "../headers/lockstep.h"

namespace fluxdb {
namespace det {

void DeterminismLinter::lock() {
    locked_ = true;
}

uint64_t DeterminismLinter::state_hash(const ecs::World& w) {
    return w.state_hash();
}

bool DeterminismLinter::states_equal(const ecs::World& a, const ecs::World& b) {
    return a.state_hash() == b.state_hash();
}

} // namespace det
} // namespace fluxdb
