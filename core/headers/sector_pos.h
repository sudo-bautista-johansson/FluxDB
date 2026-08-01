#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #19: Infinite World Origin Rebasing
//  (Sector-Relative Positions — Native FP Precision Management)
// ─────────────────────────────────────────────────────────────
// Las posiciones de mundo se particionan nativamente en un esquema
// sector-relative: `SectorPos` = (sector ID int32×3, offset local float×3).
// Nunca se materializa un float gigante de mundo: los sistemas calculan
// coordenadas al vuelo relativas a un sector de referencia (query-time) y
// la matemática local se mantiene en float32 barato y EXACTO para siempre.
//
// El grid espacial (spatial hash), la física (#12/#13) y el streaming (#20)
// usan el MISMO grid de sectores (SpatialSectorGrid).
//
// Tamaño de sector 1024 unidades: offsets locales en [-512, 512) — precisión
// sub-ulps garantizada para el rango local. Sector ID int32: ±2^31 sectores
// = ±2.2×10^12 unidades de rango de mundo (más que suficiente para "infinito").

#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>

namespace fluxdb {
namespace ecs {

using Entity = uint32_t; // alias local (header autónomo; mismo tipo que ecs.h)

constexpr float SECTOR_SIZE = 1024.0f;
constexpr float SECTOR_HALF = SECTOR_SIZE / 2.0f; // 512.0f

struct SectorPos {
    int32_t sx = 0, sy = 0, sz = 0; // sector ID (grid entero)
    float ox = 0, oy = 0, oz = 0;   // offset local en [-512, 512)

    // Decomposición world → (sector, offset). Usa double internamente para
    // que el floor/remainder sea exacto en TODO el rango de float. El sector
    // contiene el CENTRO: offset local en [-512, 512) (round-to-nearest).
    static SectorPos from_world(float x, float y, float z) {
        SectorPos p;
        double dx = x, dy = y, dz = z;
        double inv = 1.0 / static_cast<double>(SECTOR_SIZE);
        double half = static_cast<double>(SECTOR_HALF);
        p.sx = static_cast<int32_t>(std::floor((dx + half) * inv));
        p.sy = static_cast<int32_t>(std::floor((dy + half) * inv));
        p.sz = static_cast<int32_t>(std::floor((dz + half) * inv));
        p.ox = static_cast<float>(dx - static_cast<double>(p.sx) * SECTOR_SIZE);
        p.oy = static_cast<float>(dy - static_cast<double>(p.sy) * SECTOR_SIZE);
        p.oz = static_cast<float>(dz - static_cast<double>(p.sz) * SECTOR_SIZE);
        return p;
    }
    static SectorPos from_world(const float* xyz) {
        return from_world(xyz[0], xyz[1], xyz[2]);
    }

    void to_world(float& x, float& y, float& z) const {
        x = static_cast<float>(sx) * SECTOR_SIZE + ox;
        y = static_cast<float>(sy) * SECTOR_SIZE + oy;
        z = static_cast<float>(sz) * SECTOR_SIZE + oz;
    }
    void to_world(float* out) const { to_world(out[0], out[1], out[2]); }

    // Coordenadas relativas a un sector de referencia (query-time): el delta
    // de sectores × SECTOR_SIZE es un int32 → se mantiene en float32 exacto
    // para rangos locales (|delta| < 2^24 sectores = ±8M unidades).
    void to_world_relative(int32_t ref_sx, int32_t ref_sy, int32_t ref_sz,
                           float& x, float& y, float& z) const {
        x = static_cast<float>(sx - ref_sx) * SECTOR_SIZE + ox;
        y = static_cast<float>(sy - ref_sy) * SECTOR_SIZE + oy;
        z = static_cast<float>(sz - ref_sz) * SECTOR_SIZE + oz;
    }

    // Distancia exacta: deltas de sector en int64 + offsets locales en float.
    // Inmune al jitter de float de mundo (la precisión local es exacta).
    float distance_sq_to(const SectorPos& o) const {
        double ddx = (static_cast<double>(sx) - o.sx) * SECTOR_SIZE + (ox - o.ox);
        double ddy = (static_cast<double>(sy) - o.sy) * SECTOR_SIZE + (oy - o.oy);
        double ddz = (static_cast<double>(sz) - o.sz) * SECTOR_SIZE + (oz - o.oz);
        return static_cast<float>(ddx * ddx + ddy * ddy + ddz * ddz);
    }
    float distance_to(const SectorPos& o) const {
        return std::sqrt(distance_sq_to(o));
    }

    // Delta entre dos posiciones (para el codec de red #19): deltas de
    // sector (int32) + deltas de offset (float local, cuantizables).
    void delta_to(const SectorPos& o, int32_t dsec[3], float doff[3]) const {
        dsec[0] = o.sx - sx; dsec[1] = o.sy - sy; dsec[2] = o.sz - sz;
        doff[0] = o.ox - ox; doff[1] = o.oy - oy; doff[2] = o.oz - oz;
    }

    bool operator==(const SectorPos& o) const {
        return sx == o.sx && sy == o.sy && sz == o.sz &&
               ox == o.ox && oy == o.oy && oz == o.oz;
    }
    bool operator!=(const SectorPos& o) const { return !(*this == o); }
};

inline bool same_sector(const SectorPos& a, const SectorPos& b) {
    return a.sx == b.sx && a.sy == b.sy && a.sz == b.sz;
}

// ── SpatialSectorGrid: spatial hash keyed por el MISMO grid de sectores ──
// Celdas = sectores (SECTOR_SIZE); los objetos guardan solo su offset local.
// query_range visita los sectores solapados por el radio y filtra localmente
// con distancia exacta — sin jitter en mundos de tamaño arbitrario.

struct SectorKey {
    int32_t x, y, z;
    bool operator==(const SectorKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct SectorKeyHash {
    size_t operator()(const SectorKey& k) const noexcept {
        // Mezcla estilo Knuth/Triple-32: suficiente para hash tables de demo;
        // la igualdad se verifica por el operador== (sin colisiones lógicas).
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(k.x)) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.y)) * 0xC2B2AE3D27D4EB4FULL;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.z)) * 0x165667B19E3779F9ULL;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }
};

class SpatialSectorGrid {
public:
    explicit SpatialSectorGrid(float cell_size = SECTOR_SIZE) : cell_size_(cell_size) {}

    void update(Entity entity, const SectorPos& p) {
        // (Re)inserta: borra la celda vieja si cambió de sector.
        auto it = entities_.find(entity);
        if (it != entities_.end() && !same_sector(it->second, p)) {
            SectorKey old{it->second.sx, it->second.sy, it->second.sz};
            auto cit = cells_.find(old);
            if (cit != cells_.end()) cit->second.entities.erase(entity);
        }
        entities_[entity] = p;
        cells_[SectorKey{p.sx, p.sy, p.sz}].entities[entity] = p;
    }

    void remove(Entity entity) {
        auto it = entities_.find(entity);
        if (it == entities_.end()) return;
        SectorKey key{it->second.sx, it->second.sy, it->second.sz};
        auto cit = cells_.find(key);
        if (cit != cells_.end()) cit->second.entities.erase(entity);
        entities_.erase(it);
    }

    // Entidades a radio `radius` del centro (distancia exacta local).
    void query_range(const SectorPos& center, float radius,
                     std::vector<std::pair<Entity, SectorPos>>& out) const {
        int32_t r = static_cast<int32_t>(std::ceil(radius / cell_size_));
        float r2 = radius * radius;
        for (int32_t dx = -r; dx <= r; ++dx) {
            for (int32_t dy = -r; dy <= r; ++dy) {
                for (int32_t dz = -r; dz <= r; ++dz) {
                    SectorKey key{center.sx + dx, center.sy + dy, center.sz + dz};
                    auto cit = cells_.find(key);
                    if (cit == cells_.end()) continue;
                    for (const auto& [e, p] : cit->second.entities) {
                        if (p.distance_sq_to(center) <= r2) {
                            out.emplace_back(e, p);
                        }
                    }
                }
            }
        }
    }

    const SectorPos* find(Entity entity) const {
        auto it = entities_.find(entity);
        return it == entities_.end() ? nullptr : &it->second;
    }

    size_t entity_count() const { return entities_.size(); }
    size_t cell_count() const { return cells_.size(); }

private:
    float cell_size_;
    struct Cell {
        std::unordered_map<Entity, SectorPos> entities;
    };
    std::unordered_map<SectorKey, Cell, SectorKeyHash> cells_;
    std::unordered_map<Entity, SectorPos> entities_;
};

} // namespace ecs
} // namespace fluxdb
