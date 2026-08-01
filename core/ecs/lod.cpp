#include "../headers/lod.h"
#include <algorithm>

namespace fluxdb {
namespace lod {

namespace {
const LODRule kDefaultFull{1e30f, 1, 0.0f};
const LODRule kNone{0.0f, 1, 0.0f};
} // namespace

void LodManager::set_component_rules(uint8_t comp_id, const std::vector<LODRule>& rules) {
    if (comp_id >= MAX_LOD_COMPONENTS) return;
    CompRules& cr = comps_[comp_id];
    cr.count = 0;
    size_t n = std::min<size_t>(rules.size(), MAX_LOD_TIERS);
    for (size_t i = 0; i < n; ++i) {
        cr.rules[i] = rules[i];
        ++cr.count;
    }
}

bool LodManager::has_rules(uint8_t comp_id) const {
    return comp_id < MAX_LOD_COMPONENTS && comps_[comp_id].count > 0;
}

Tier LodManager::tier_for(uint8_t comp_id, float distance) const {
    if (comp_id >= MAX_LOD_COMPONENTS) return Tier::FULL;
    const CompRules& cr = comps_[comp_id];
    if (cr.count == 0) return Tier::FULL;
    for (uint8_t i = 0; i < cr.count; ++i) {
        if (distance <= cr.rules[i].max_distance) {
            return static_cast<Tier>(i);
        }
    }
    return Tier::NONE; // más lejos que el último rango
}

const LODRule& LodManager::rule_for(uint8_t comp_id, float distance) const {
    Tier t = tier_for(comp_id, distance);
    if (t == Tier::NONE) return kNone;
    const LODRule* r = rule_for_tier(comp_id, t);
    return r ? *r : kDefaultFull;
}

const LODRule* LodManager::rule_for_tier(uint8_t comp_id, Tier t) const {
    if (comp_id >= MAX_LOD_COMPONENTS) return nullptr;
    const CompRules& cr = comps_[comp_id];
    uint8_t idx = static_cast<uint8_t>(t);
    if (idx >= cr.count) return nullptr;
    return &cr.rules[idx];
}

bool LodManager::should_update(uint8_t comp_id, uint32_t entity, Tier t,
                               uint32_t last_write_tick, uint64_t current_tick) const {
    if (last_write_tick == 0 || t == Tier::NONE) return false;
    const LODRule* r = rule_for_tier(comp_id, t);
    uint16_t every = r ? r->every_n_ticks : 1;

    uint64_t key = (static_cast<uint64_t>(comp_id) << 40) |
                   (static_cast<uint64_t>(t) << 32) |
                   static_cast<uint64_t>(entity);
    auto it = last_sent_.find(key);
    if (it == last_sent_.end()) {
        return true; // nunca enviado en este tier: enviar
    }
    // Solo si hay cambios pendientes respecto al watermark Y pasó el intervalo
    return static_cast<uint64_t>(last_write_tick) > it->second &&
           current_tick - it->second >= every;
}

void LodManager::mark_sent(uint8_t comp_id, uint32_t entity, Tier t, uint64_t tick) {
    if (t == Tier::NONE) return;
    uint64_t key = (static_cast<uint64_t>(comp_id) << 40) |
                   (static_cast<uint64_t>(t) << 32) |
                   static_cast<uint64_t>(entity);
    last_sent_[key] = tick;
}

void LodManager::quantize(uint8_t comp_id, Tier t, uint8_t* data, size_t size) const {
    if (size == 0 || size % 4 != 0) return; // no es un array de float32
    const LODRule* r = rule_for_tier(comp_id, t);
    float step = r ? r->step : 0.0f;
    if (step <= 0.0f) return;

    float* f = reinterpret_cast<float*>(data);
    size_t n = size / 4;
    for (size_t i = 0; i < n; ++i) {
        f[i] = std::round(f[i] / step) * step;
    }
}

} // namespace lod
} // namespace fluxdb
