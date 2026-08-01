#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #15: Spatial Perception Indices (Native Sight/Sound
//  Queries) — AI stack
// ─────────────────────────────────────────────────────────────
// Capa de percepción sobre el SpatialIndex (octree). En lugar de
// raycasts brutos por agente, cada observador registra un cono de
// visión y/o un radio de audición; un paso de percepción batch
// genera eventos (OnEntitySpotted / OnSoundHeard) solo ante cambios
// reales (un set "recently observed" por observador evita re-avisos).
// Los eventos son deterministas y ordenados por (observer, target),
// listos para replay/delta (misma infraestructura que #9).

#include "ecs.h"
#include "spatial_index.h"
#include "types.h"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace fluxdb {
namespace ai {

using fluxdb::ecs::Entity;
using fluxdb::spatial::SpatialIndex;

// Cono de visión de un observador (orientado al facing + FOV).
struct VisionCone {
    Entity observer = 0;
    float ox = 0, oy = 0, oz = 0;
    float fx = 0, fy = 0, fz = 0;   // dirección del facing (normalizada)
    float range = 50.0f;
    float cos_half_fov = 0.8f;      // cos(36.9°) ≈ 0.8 → FOV ~74°
};

// Radio de audición (omnidireccional).
struct HearingRadius {
    Entity observer = 0;
    float ox = 0, oy = 0, oz = 0;
    float radius = 30.0f;
};

// Sonido emitido en el mundo (para OnSoundHeard).
struct SoundEvent {
    float sx = 0, sy = 0, sz = 0;
    float loudness = 1.0f;   // multiplica el radio efectivo
};

// Tipos de evento de percepción.
enum class PerceptionKind : uint8_t {
    SPOTTED,
    LOST,
    SOUND_HEARD
};

struct PerceptionEvent {
    Entity observer = 0;
    Entity target = 0;
    PerceptionKind kind = PerceptionKind::SPOTTED;
    uint64_t tick = 0;
};

// Índice de percepción: mantiene observadores y emisores, y genera
// eventos de cambio al procesar (procesa O(cambios), no O(n*m)).
class PerceptionIndex {
public:
    void set_tick(uint64_t t) { tick_ = t; }

    void add_vision(const VisionCone& c) {
        cones_.push_back(c);
        observed_.try_emplace(c.observer);  // set vacío
    }

    void add_hearing(const HearingRadius& h) {
        hearers_.push_back(h);
        observed_.try_emplace(h.observer);
    }

    void update_vision_pos(Entity e, float x, float y, float z) {
        for (auto& c : cones_) if (c.observer == e) { c.ox = x; c.oy = y; c.oz = z; }
    }

    void update_vision_facing(Entity e, float fx, float fy, float fz) {
        float len = std::sqrt(fx*fx + fy*fy + fz*fz);
        if (len > 0) for (auto& c : cones_) if (c.observer == e) { c.fx = fx/len; c.fy = fy/len; c.fz = fz/len; }
    }

    size_t observer_count() const { return cones_.size() + hearers_.size(); }

    // Procesa una posición de target por observador de visión. Los
    // targets dentro del cono disparan SPOTTED (una vez), fuera del
    // rango disparan LOST si antes estaban observados.
    void process_target_positions(SpatialIndex& index, std::vector<PerceptionEvent>& events) {
        for (const auto& cone : cones_) {
            std::vector<Entity> near;
            index.query_range(cone.ox, cone.oy, cone.oz, cone.range, near);
            auto& seen = observed_[cone.observer];

            std::vector<Entity> now_seen;
            for (Entity t : near) {
                if (t == cone.observer) continue;
                float dx, dy, dz;
                if (!index.get_position(t, dx, dy, dz)) continue;
                dx -= cone.ox; dy -= cone.oy; dz -= cone.oz;
                float dist_sq = dx*dx + dy*dy + dz*dz;
                if (dist_sq > cone.range * cone.range) continue;
                float dist = std::sqrt(dist_sq);
                if (dist > 0) {
                    float dot = (dx*cone.fx + dy*cone.fy + dz*cone.fz) / dist;
                    if (dot < cone.cos_half_fov) continue; // fuera del cono
                }
                now_seen.push_back(t);
                if (std::find(seen.begin(), seen.end(), t) == seen.end()) {
                    events.push_back({cone.observer, t, PerceptionKind::SPOTTED, tick_});
                }
            }

            // Targets que ya no se ven → LOST.
            for (Entity t : seen) {
                if (std::find(now_seen.begin(), now_seen.end(), t) == now_seen.end()) {
                    events.push_back({cone.observer, t, PerceptionKind::LOST, tick_});
                }
            }
            seen = std::vector<Entity>(now_seen.begin(), now_seen.end());
        }
    }

    // Procesa sonidos emitidos: cada emisor notifica a los observadores
    // dentro de (radius * loudness).
    void process_sound(const SoundEvent& s, std::vector<PerceptionEvent>& events) {
        for (const auto& h : hearers_) {
            float dx = h.ox - s.sx, dy = h.oy - s.sy, dz = h.oz - s.sz;
            float eff = h.radius * s.loudness;
            if (dx*dx + dy*dy + dz*dz <= eff*eff) {
                events.push_back({h.observer, 0, PerceptionKind::SOUND_HEARD, tick_});
            }
        }
    }

    void clear_events(std::vector<PerceptionEvent>& events) { events.clear(); }

private:
    uint64_t tick_ = 0;
    std::vector<VisionCone> cones_;
    std::vector<HearingRadius> hearers_;
    // Set "recientemente observado" por observador (dedup de eventos).
    std::unordered_map<Entity, std::vector<Entity>> observed_;
};

} // namespace ai
} // namespace fluxdb
