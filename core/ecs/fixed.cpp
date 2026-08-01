#include "../headers/fixed.h"
#include <cmath>

namespace fluxdb {
namespace det {

const SinTable& SinTable::get() {
    static const SinTable table;
    return table;
}

SinTable::SinTable() {
    // Entradas i: sin(i × (π/2)/1024), i ∈ [0, 1024]. std::sin es
    // IEEE-754 determinista → la misma tabla en cualquier plataforma.
    const double half_pi = 1.57079632679489661923;
    for (int i = 0; i <= 1024; ++i) {
        table_[i] = static_cast<int32_t>(std::lround(std::sin(half_pi * i / 1024.0) * 65536.0));
    }
}

} // namespace det
} // namespace fluxdb
