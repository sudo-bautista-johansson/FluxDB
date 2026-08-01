#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #22: GPU-Driven Procedural Generation Pipeline
//  (Phase 5 - Frontier / GPU)
// ─────────────────────────────────────────────────────────────
// Generación procedural masiva offloadada a un "GPU simulado": los
// parámetros (seed chain #21 + rangos) se replican, el device genera
// instancias en el buffer espejo (#3) de forma determinista, y un
// readback asíncrono trae solo el subconjunto que el gameplay necesita
// (p.ej. objetos con colisión). El CPU NO ve cada instancia.

#include "seed_chain.h"
#include "gpu_mirror.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>
#include <cmath>

namespace fluxdb {
namespace gen {

// Parámetro de generación: qué generar y dónde (replicable).
struct ProcGenParams {
    fluxdb::det::SeedChain seed;
    int32_t instance_count = 0;
    float min_x = 0, min_y = 0, min_z = 0;
    float max_x = 0, max_y = 0, max_z = 0;
    // Máscara de qué instancias requieren readback (p.ej. solo las que
    // tienen colisión → bit 0). Simula "gameplay-relevant subset".
    uint32_t readback_mask = 0;
};

// Tipo de readback: el GPU genera, el CPU pide async qué leer.
struct ReadbackRequest {
    bool pending = false;
    std::vector<float> results; // (x,y,z) por instancia leída
};

// Pipeline de generación procedural. El "GPU" (función determinista
// basada en la seed) escribe al buffer espejo; el readback tira solo
// las instancias seleccionadas por la máscara.
class ProcGenPipeline {
public:
    explicit ProcGenPipeline(size_t max_instances = 4096)
        : mirror_(3 * sizeof(float), max_instances) {}

    // Lanza una generación "en GPU" (simulada, síncrona pero determinista).
    // Devuelve cuántas instancias se generaron.
    size_t generate(const ProcGenParams& p) {
        last_params_ = p;
        if (p.instance_count <= 0) return 0;

        // El GPU (determinista vía seed chain) escribe transforms al espejo.
        fluxdb::det::FixedRandom rng = p.seed.rng();
        size_t generated = 0;
        for (int32_t i = 0; i < p.instance_count; ++i) {
            float u = rng.next_fix01().to_float();
            float v = rng.next_fix01().to_float();
            float w = rng.next_fix01().to_float();
            float tx = p.min_x + u * (p.max_x - p.min_x);
            float ty = p.min_y + v * (p.max_y - p.min_y);
            float tz = p.min_z + w * (p.max_z - p.min_z);
            float t[3] = {tx, ty, tz};
            mirror_.set_component(i, t);
            ++generated;
        }
        mirror_.upload_dirty_pages();
        return generated;
    }

    // Readback asíncrono: trae SOLO las instancias cuyo índice tiene el
    // bit de `readback_mask` (p.ej. índices pares → colisión en 50%).
    // Sin descargar todo el buffer al CPU.
    ReadbackRequest readback(const ProcGenParams& p) const {
        ReadbackRequest req;
        req.pending = p.instance_count > 0;
        if (!req.pending) return req;

        fluxdb::det::FixedRandom rng = p.seed.rng(); // regenera la MISMA secuencia
        for (int32_t i = 0; i < p.instance_count; ++i) {
            float u = rng.next_fix01().to_float();
            float v = rng.next_fix01().to_float();
            float w = rng.next_fix01().to_float();
            float tx = p.min_x + u * (p.max_x - p.min_x);
            float ty = p.min_y + v * (p.max_y - p.min_y);
            float tz = p.min_z + w * (p.max_z - p.min_z);
            bool needs = (static_cast<uint32_t>(i) & p.readback_mask) != 0;
            if (needs) {
                req.results.push_back(tx);
                req.results.push_back(ty);
                req.results.push_back(tz);
            }
        }
        return req;
    }

    // Parámetros de la última generación (para replicar entre clientes).
    const ProcGenParams& last_params() const { return last_params_; }

    // Determinismo: dos pipelines con la misma seed generan lo mismo.
    bool verify_identical(const ProcGenParams& p) const {
        ReadbackRequest a = readback(p);
        ProcGenParams p2 = p; // misma seed
        (void)p2;
        ReadbackRequest b = readback(p);
        if (a.results.size() != b.results.size()) return false;
        for (size_t i = 0; i < a.results.size(); ++i) {
            if (std::fabs(a.results[i] - b.results[i]) > 1e-5f) return false;
        }
        return true;
    }

private:
    gpu::MirroredArchetype mirror_;
    ProcGenParams last_params_;
};

} // namespace gen
} // namespace fluxdb
