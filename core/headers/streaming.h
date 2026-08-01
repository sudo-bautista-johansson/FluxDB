#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #20: Priority-Based Chunk Streaming with Predictive
//  Prefetch (Streaming & Massive Worlds)
// ─────────────────────────────────────────────────────────────
// Los chunks (células del grid de sectores #19) tienen un estado
// DORMANT / LOADING / ACTIVE. La prioridad de streaming se computa
// desde distancia + posición extrapolada por VELOCIDAD del jugador
// (prefetch predictivo) + frustum de cámara. Los sistemas de juego
// pueden filtrar entidades DORMANT a coste ~0 vía chunk-level check.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace fluxdb {
namespace streaming {

// Estado de streaming de un chunk (componente en la entidad chunk).
enum class ChunkState : uint8_t {
    DORMANT = 0,
    LOADING = 1,
    ACTIVE = 2
};

// Política de prioridad de streaming (#20).
struct StreamPriority {
    float distance_weight = 1.0f;        // peso de la distancia actual
    float velocity_weight = 2.0f;        // peso del prefetch por velocidad
    float frustum_weight = 3.0f;         // peso de estar en el frustum
    float lookahead_seconds = 2.0f;      // horizonte de extrapolación
    float load_radius = 1000.0f;         // radio de carga (ACTIVE)
    float prefetch_radius = 1600.0f;     // radio de prefetch (LOADING)
    float max_load_budget = 16.0f;       // chunks a cargar por update
};

// Representa el "jugador/cámara" que conduce el streaming.
struct StreamObserver {
    float x = 0, y = 0, z = 0;         // posición actual
    float vx = 0, vy = 0, vz = 0;      // velocidad (para extrapolación)
    float fx = 0, fy = 0, fz = -1.0f;  // forward de la cámara (frustum)
    float hfov = 1.0472f, vfov = 0.7854f; // ~60°x45°

    // Posición extrapolada dentro de `lookahead` segundos.
    void extrapolate(float seconds, float& ex, float& ey, float& ez) const {
        ex = x + vx * seconds;
        ey = y + vy * seconds;
        ez = z + vz * seconds;
    }

    // ¿Un punto está dentro del frustum (aproximación por cono)?
    bool in_frustum(float px, float py, float pz, float radius) const {
        float vx = px - x, vy = py - y, vz = pz - z;
        float dist = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (dist <= radius) return true;
        float dot = (vx * fx + vy * fy + vz * fz) / (dist > 1e-6f ? dist : 1e-6f);
        return dot > std::cos(hfov * 0.5f);
    }
};

// Un chunk (celda) del mundo streaming. key = (cell_x, cell_y, cell_z).
struct StreamChunk {
    int32_t cx = 0, cy = 0, cz = 0;
    ChunkState state = ChunkState::DORMANT;
    float priority = 0.0f;      // prioridad actual (mayor = más urgente)
    uint64_t last_touched_tick = 0;
};

inline uint64_t chunk_key(int32_t x, int32_t y, int32_t z) {
    // Empaqueta 3×int32 en un uint64 con XOR de bits altos.
    uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) ^
                 (static_cast<uint64_t>(static_cast<uint32_t>(y)) & 0xFFFFFFFFULL);
    h = h * 0x9E3779B97F4A7C15ULL;
    return h ^ (static_cast<uint64_t>(static_cast<uint32_t>(z)) & 0xFFFFFFFFULL);
}

// Manager de streaming: decide qué chunks cargar/prefetchear.
class StreamingManager {
public:
    StreamingManager() = default;

    // Registra el chunk si no existía (default DORMANT).
    StreamChunk* ensure_chunk(int32_t cx, int32_t cy, int32_t cz) {
        uint64_t k = chunk_key(cx, cy, cz);
        auto it = chunks_.find(k);
        if (it != chunks_.end()) return &it->second;
        StreamChunk c;
        c.cx = cx; c.cy = cy; c.cz = cz;
        return &chunks_.emplace(k, c).first->second;
    }

    void set_chunk_state(int32_t cx, int32_t cy, int32_t cz, ChunkState s) {
        if (StreamChunk* c = ensure_chunk(cx, cy, cz)) c->state = s;
    }

    const StreamChunk* get_chunk(int32_t cx, int32_t cy, int32_t cz) const {
        auto it = chunks_.find(chunk_key(cx, cy, cz));
        return it == chunks_.end() ? nullptr : &it->second;
    }

    // Ronda de streaming: recalcula prioridades y marca chunks a cargar.
    // Llena `to_load` (chunks LOADING/ACTIVE por prioridad, respetando el
    // presupuesto) y `to_unload` (chunks que quedan fuera del radio).
    void update(const StreamObserver& obs, uint64_t tick) {
        std::vector<std::pair<float, StreamChunk*>> scored;
        scored.reserve(chunks_.size());

        for (auto& [k, c] : chunks_) {
            // Distancia al chunk (centro de la celda).
            float dist = std::sqrt(
                (c.cx - obs.x / 1024.0f) * (c.cx - obs.x / 1024.0f) +
                (c.cy - obs.y / 1024.0f) * (c.cy - obs.y / 1024.0f) +
                (c.cz - obs.z / 1024.0f) * (c.cz - obs.z / 1024.0f)) * 1024.0f;

            // Prefetch predictivo: distancia al punto extrapolado por
            // velocidad del observador (horizonte = lookahead_seconds).
            float ex, ey, ez;
            obs.extrapolate(lookahead_seconds, ex, ey, ez);
            float predict_dist = std::sqrt(
                (c.cx - ex / 1024.0f) * (c.cx - ex / 1024.0f) +
                (c.cy - ey / 1024.0f) * (c.cy - ey / 1024.0f) +
                (c.cz - ez / 1024.0f) * (c.cz - ez / 1024.0f)) * 1024.0f;

            bool in_frust = obs.in_frustum(c.cx * 1024.0f, c.cy * 1024.0f, c.cz * 1024.0f, 512.0f);

            // Prioridad: cercanía + velocidad (predicción) + frustum.
            float prio = 0.0f;
            if (dist < load_radius_) prio += load_radius_ - dist;
            prio += (prefetch_radius_ - predict_dist) * 0.5f;
            if (in_frust) prio += frustum_bonus_;
            if (c.state == ChunkState::ACTIVE) prio += active_bonus_;
            c.priority = std::max(0.0f, prio);
            c.last_touched_tick = tick;
            scored.emplace_back(c.priority, &c);
        }

        // Ordena por prioridad descendente.
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        to_load_.clear();
        to_unload_.clear();
        size_t budget = max_load_budget_;
        for (auto& [prio, c] : scored) {
            float dist = std::sqrt(
                (c->cx - obs.x / 1024.0f) * (c->cx - obs.x / 1024.0f) +
                (c->cy - obs.y / 1024.0f) * (c->cy - obs.y / 1024.0f) +
                (c->cz - obs.z / 1024.0f) * (c->cz - obs.z / 1024.0f)) * 1024.0f;
            float ex, ey, ez;
            obs.extrapolate(lookahead_seconds, ex, ey, ez);
            float predict_dist = std::sqrt(
                (c->cx - ex / 1024.0f) * (c->cx - ex / 1024.0f) +
                (c->cy - ey / 1024.0f) * (c->cy - ey / 1024.0f) +
                (c->cz - ez / 1024.0f) * (c->cz - ez / 1024.0f)) * 1024.0f;

            // Carga si está en el radio de carga O el observador está
            // por entrar (prefetch predictivo por velocidad).
            if (dist <= load_radius_ || predict_dist <= load_radius_) {
                if (c->state != ChunkState::ACTIVE && budget > 0) {
                    to_load_.push_back(c);
                    --budget;
                }
            } else if (dist > prefetch_radius_ && predict_dist > prefetch_radius_) {
                if (c->state == ChunkState::ACTIVE) to_unload_.push_back(c);
            }
        }
    }

    // Chunks a cargar (orden de prioridad) tras la última update().
    const std::vector<StreamChunk*>& to_load() const { return to_load_; }
    // Chunks a descargar (fuera del radio de prefetch).
    const std::vector<StreamChunk*>& to_unload() const { return to_unload_; }

    size_t active_count() const {
        size_t n = 0;
        for (const auto& [k, c] : chunks_) if (c.state == ChunkState::ACTIVE) ++n;
        return n;
    }
    size_t total_count() const { return chunks_.size(); }

    void set_radii(float load, float prefetch) { load_radius_ = load; prefetch_radius_ = prefetch; }
    void set_max_load(size_t n) { max_load_budget_ = n; }

private:
    std::unordered_map<uint64_t, StreamChunk> chunks_;
    float load_radius_ = 1000.0f;
    float prefetch_radius_ = 1600.0f;
    size_t max_load_budget_ = 16;
    float frustum_bonus_ = 500.0f;
    float active_bonus_ = 100.0f;
    float lookahead_seconds = 2.0f;
    std::vector<StreamChunk*> to_load_;
    std::vector<StreamChunk*> to_unload_;
};

} // namespace streaming
} // namespace fluxdb
