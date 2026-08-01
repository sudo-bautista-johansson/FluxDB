#pragma once

#include <cstdint>
#include <functional>
#include "../headers/fixed.h"

namespace fluxdb {
namespace det {

// ── SeedChain (#21) ──────────────────────────────────────────
// Cada entidad lleva un SeedChain: una semilla jerárquica derivada
// determinísticamente de (world_seed, sector_id, local_index) vía
// counter-based PRNG (Philox-style con xorshift64*). Sin estado
// mutante global: la generación es trivially paralelizable y da
// secuencias idénticas en cualquier máquina sin sincronización.
//
// Los valores generados son reorden-independentes (no hay estado
// mutable compartido), perfecto para procedural generation en
// lockstep (#11) o red (#8).

struct SeedChain {
    int32_t sector_x = 0, sector_y = 0, sector_z = 0;
    uint32_t local_index = 0;
    uint64_t world_seed = 0;

    uint64_t chain_seed() const {
        // Mezcla determinista: world_seed + sector hash + local_index
        uint64_t h = world_seed;
        h ^= static_cast<uint64_t>(sector_x) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<uint64_t>(sector_y) * 0xC6A4A7935BD1E995ULL;
        h ^= static_cast<uint64_t>(sector_z) * 0xBF58476D1CE4E5B9ULL;
        h ^= static_cast<uint64_t>(local_index) * 0x9E3779B9ULL;
        return h;
    }

    FixedRandom rng() const {
        return FixedRandom(chain_seed());
    }

    // Herencia: sub-seed para entidades generadas por esta entidad
    uint64_t child_seed(uint32_t child_index) const {
        return chain_seed() ^ (static_cast<uint64_t>(child_index + 1) * 0x9E3779B97F4A7C15ULL);
    }
};

inline bool operator==(const SeedChain& a, const SeedChain& b) {
    return a.sector_x == b.sector_x && a.sector_y == b.sector_y &&
           a.sector_z == b.sector_z && a.local_index == b.local_index &&
           a.world_seed == b.world_seed;
}
inline bool operator!=(const SeedChain& a, const SeedChain& b) { return !(a == b); }

} // namespace det
} // namespace fluxdb

namespace std {
template<> struct hash<fluxdb::det::SeedChain> {
    size_t operator()(const fluxdb::det::SeedChain& s) const noexcept {
        return static_cast<size_t>(s.chain_seed());
    }
};
} // namespace std