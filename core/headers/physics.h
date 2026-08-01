#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #13: Time-Travel Collision Queries
//  (Native physics queries against historical world state)
// ─────────────────────────────────────────────────────────────
// Ray/volume queries contra el estado del mundo en un tick pasado,
// reusando el HistoryManager (#4) para reconstruir posiciones sin
// modificar el World. La misma API funciona en vivo (tick = ahora).

#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>

namespace fluxdb {
namespace physics {

// Un rayo: origen + dirección (NO normalizada, la distancia es en unidades).
struct Ray {
    float ox = 0, oy = 0, oz = 0;
    float dx = 0, dy = 0, dz = -1;

    // Intersección rayo vs esfera (broadphase físico). `r2` = radio².
    // Devuelve la distancia al punto de entrada más cercano o -1 si no hay.
    float intersect_sphere(float cx, float cy, float cz, float r2) const {
        float ocx = cx - ox, ocy = cy - oy, ocz = cz - oz;
        float l = ocx * dx + ocy * dy + ocz * dz;
        if (l < 0) return -1.0f; // la esfera está detrás del origen
        float oc2 = ocx * ocx + ocy * ocy + ocz * ocz;
        float d2 = oc2 - l * l;
        if (d2 > r2) return -1.0f; // el rayo pasa de largo
        float t = std::sqrt(r2 - d2);
        return l - t;
    }

    // Intersección rayo vs AABB (slab test), con distancia máxima.
    float intersect_aabb(float minx, float miny, float minz,
                         float maxx, float maxy, float maxz) const {
        float tmin = 0.0f, tmax = std::numeric_limits<float>::max();
        float inv[3] = { 1.0f / (dx != 0.0f ? dx : 1e-30f),
                         1.0f / (dy != 0.0f ? dy : 1e-30f),
                         1.0f / (dz != 0.0f ? dz : 1e-30f) };
        float o[3] = { ox, oy, oz };
        float mn[3] = { minx, miny, minz }, mx[3] = { maxx, maxy, maxz };
        for (int a = 0; a < 3; ++a) {
            float t1 = (mn[a] - o[a]) * inv[a];
            float t2 = (mx[a] - o[a]) * inv[a];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1.0f;
        }
        return tmin;
    }
};

// Resultado de un raycast histórico/en vivo.
struct RaycastHit {
    uint32_t entity = 0;
    float distance = 0;
    float x = 0, y = 0, z = 0; // punto de impacto
    bool hit = false;
};

// Forma de volumen para queries históricas.
enum class VolumeShape2 {
    SPHERE,
    AABB
};

struct VolumeQuery {
    VolumeShape2 shape = VolumeShape2::SPHERE;
    float cx = 0, cy = 0, cz = 0;
    float r = 10.0f;             // SPHERE
    float max_x = 0, max_y = 0, max_z = 0; // AABB
};

} // namespace physics
} // namespace fluxdb
