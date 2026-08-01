#pragma once

// ─────────────────────────────────────────────────────────────
//  Bandwidth-Aware Component LOD (#10)
// ─────────────────────────────────────────────────────────────
// Los componentes declaran tiers de replicación LOD por distancia
// (p.ej. Full: 0-20m, Reduced: 20-100m, Minimal: 100m+) con reglas de
// frecuencia de actualización y cuantización. El builder de deltas del
// suscriptor (#9 + #10) elige el tier por (entity, subscriber) según la
// distancia entre la entidad y el centro del volumen de interés, y ajusta
// frecuencia y precisión en consecuencia.
//
// Cuantización: paso `step` aplicado a los campos float32 del componente
// (size % 4 == 0); step 0 = sin cuantizar.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <unordered_map>
#include <cmath>

namespace fluxdb {
namespace lod {

// Tier de replicación por distancia.
enum class Tier : uint8_t {
    FULL = 0,
    REDUCED = 1,
    MINIMAL = 2,
    NONE = 3, // fuera del último rango: no se replica
};

constexpr size_t MAX_LOD_TIERS = 3;
constexpr size_t MAX_LOD_COMPONENTS = 64; // == ecs::MAX_COMPONENTS

struct LODRule {
    float max_distance = 0.0f;  // distancia máxima a la que aplica este tier
    uint16_t every_n_ticks = 1; // frecuencia: 1 = todos los ticks
    float step = 0.0f;          // paso de cuantización float32 (0 = sin cuantizar)
};

class LodManager {
public:
    // Reglas del componente: 1..3 tiers ascendentes por max_distance.
    // Los componentes sin reglas replican FULL sin cuantizar cada tick.
    void set_component_rules(uint8_t comp_id, const std::vector<LODRule>& rules);

    bool has_rules(uint8_t comp_id) const;

    // Tier que aplica a `distance`. NONE si la distancia supera el último
    // rango; FULL si el componente no tiene reglas.
    Tier tier_for(uint8_t comp_id, float distance) const;

    // Regla del tier activo a `distance` (FULL por defecto).
    const LODRule& rule_for(uint8_t comp_id, float distance) const;

    // Rate limit por (comp, entity, tier): true si hay cambios pendientes
    // (last_write > watermark) Y pasó el intervalo del tier.
    bool should_update(uint8_t comp_id, uint32_t entity, Tier t,
                       uint32_t last_write_tick, uint64_t current_tick) const;

    // Sella el watermark del (comp, entity, tier) con el tick actual.
    void mark_sent(uint8_t comp_id, uint32_t entity, Tier t, uint64_t tick);

    // Cuantiza in-place los campos float32 del componente (size % 4 == 0)
    // con el paso del tier activo de ESE componente. step 0 = sin cambios.
    void quantize(uint8_t comp_id, Tier t, uint8_t* data, size_t size) const;

private:
    struct CompRules {
        std::array<LODRule, MAX_LOD_TIERS> rules{};
        uint8_t count = 0;
    };

    const LODRule* rule_for_tier(uint8_t comp_id, Tier t) const;

    std::array<CompRules, MAX_LOD_COMPONENTS> comps_{};
    // Watermark del último envío por (comp, entity, tier):
    // [comp 8 bits][tier 2 bits][entity 32 bits] -> tick
    std::unordered_map<uint64_t, uint64_t> last_sent_;
};

} // namespace lod
} // namespace fluxdb
