#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #34: Causal Divergence Tracing ("Why Did We Desync")
//  (Phase 6 - Flagship Capstone)
// ─────────────────────────────────────────────────────────────
// Desyncs de red/replay: cuando dos máquinas divergen, este módulo no
// solo reporta el tick — caminata ATRÁS por el historial causal de
// escrituras (#4 tick-stamps + #7 delta + #32 write attribution) para
// hallar el PRIMER write divergente y la cadena de {sistema, tick,
// componente, input} que lo produjo.
//
// Modelo: cada máquina (replica) mantiene un log de writes tick-stamped
// con su componente de entrada (read set). Checksums de chunk
// incrementales detectan el desync; el trace-back sigue los inputs
// hacia atrás hasta la causa raíz.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace fluxdb {
namespace div {

// Un write grabado: quién, a qué, cuándo, qué leyó.
struct WriteRecord {
    uint64_t tick = 0;
    uint32_t system = 0;        // hash del nombre de sistema
    std::string system_name;
    uint32_t entity = 0;
    uint32_t component = 0;     // hash del nombre de componente
    std::string component_name;
    float old_value = 0;
    float new_value = 0;
    uint32_t input_component = 0; // qué componente leyó como entrada (0 = none)
    std::string input_name;
    float input_value = 0;        // valor de esa entrada en ESTE tick
};

// Eslabón de la cadena causal presentada al usuario.
struct CausalLink {
    uint64_t tick = 0;
    std::string system;
    std::string component;
    std::string entity_desc;
    float local_value = 0;
    float remote_value = 0;
    bool value_diverges = false;  // el propio valor difiere entre máquinas
    std::string input_read;       // "Armor" o "" — el input causante
    float input_local = 0, input_remote = 0;
};

struct CausalChain {
    bool found = false;
    uint64_t first_divergent_tick = 0;
    std::string root_cause;
    std::vector<CausalLink> links; // orden causal: raíz → ... → superficie
};

struct DivergenceReport {
    bool diverged = false;
    uint64_t tick = 0;
    std::string component_chunk; // nombre de chunk (componente) que divergió
};

// Una réplica de la simulación (una máquina).
class Replica {
public:
    // Graba un write. `input_component`/`input_name`/`input_value` = el read
    // set del sistema en ese tick (lo que el sistema leyó para decidir).
    void record_write(uint64_t tick, const std::string& system_name,
                      uint32_t entity, const std::string& component_name,
                      float old_value, float new_value,
                      const std::string& input_name, float input_value) {
        WriteRecord w;
        w.tick = tick;
        w.system = fnv(system_name);
        w.system_name = system_name;
        w.entity = entity;
        w.component = fnv(component_name);
        w.component_name = component_name;
        w.old_value = old_value;
        w.new_value = new_value;
        w.input_component = input_name.empty() ? 0 : fnv(input_name);
        w.input_name = input_name;
        w.input_value = input_value;
        log_.push_back(w);
        // Checksum incremental del chunk de ese componente POR TICK (solo el
        // touched). Distintas escrituras en el mismo tick se XORan juntas;
        // cada tick tiene su propia entrada → comparaciones tick a tick.
        uint32_t fbits;
        float fv = w.new_value == 0 ? 1.0f : w.new_value;
        std::memcpy(&fbits, &fv, sizeof(fbits));
        chunk_tick_checksums_[w.component][w.tick] ^=
            static_cast<uint64_t>(fbits) * hash_mix(w.tick, w.entity);
        component_names_[w.component] = component_name;
    }

    size_t write_count() const { return log_.size(); }

    const std::vector<WriteRecord>& log() const { return log_; }

    // Checksum del chunk (componente) en el tick dado. 0 si no se tocó.
    uint64_t chunk_checksum_at(uint32_t component, uint64_t tick) const {
        auto it = chunk_tick_checksums_.find(component);
        if (it == chunk_tick_checksums_.end()) return 0;
        auto it2 = it->second.find(tick);
        return it2 == it->second.end() ? 0 : it2->second;
    }

    // Nombre del componente a partir de su hash (último nombre visto).
    std::string component_name_of(uint32_t component) const {
        auto it = component_names_.find(component);
        return it == component_names_.end() ? std::string() : it->second;
    }

private:
    static uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ULL; }
        return h;
    }
    static uint64_t hash_mix(uint64_t a, uint64_t b) {
        a += 0x9E3779B97F4A7C15ULL;
        a = (a ^ (a >> 30)) * 0xBF58476D1CE4E5B9ULL;
        a = (a ^ (a >> 27)) * 0x94D049BB133111EBULL;
        return a ^ (a >> 31) ^ b;
    }

    std::vector<WriteRecord> log_;
    std::unordered_map<uint32_t, std::unordered_map<uint64_t, uint64_t>> chunk_tick_checksums_;
    std::unordered_map<uint32_t, std::string> component_names_;
};

// Motor de detección + trace-back causal.
class CausalDivergenceTracer {
public:
    // Compara los checksums de chunks entre dos réplicas tick a tick.
    // Avanza cada réplica por TODAS las escrituras de cada tick y compara
    // los chunks tocados en ese tick — detecta divergencia aunque una
    // réplica tenga writes extra en el mismo tick.
    DivergenceReport find_first_divergence(const Replica& a, const Replica& b) const {
        DivergenceReport rep;
        size_t i = 0, j = 0;
        while (i < a.write_count() || j < b.write_count()) {
            // Siguiente tick no procesado en cada réplica.
            uint64_t ta = i < a.write_count() ? a.log()[i].tick : UINT64_MAX;
            uint64_t tb = j < b.write_count() ? b.log()[j].tick : UINT64_MAX;
            uint64_t tick = (ta < tb) ? ta : tb;
            if (tick == UINT64_MAX) break;

            // Consumir todas las escrituras de `tick` en ambas réplicas.
            std::vector<uint32_t> touched;
            while (i < a.write_count() && a.log()[i].tick == tick) {
                touched.push_back(a.log()[i].component);
                ++i;
            }
            while (j < b.write_count() && b.log()[j].tick == tick) {
                touched.push_back(b.log()[j].component);
                ++j;
            }
            // Comparar cada chunk tocado en este tick (checksums del tick).
            for (uint32_t comp : touched) {
                uint64_t ca = a.chunk_checksum_at(comp, tick);
                uint64_t cb = b.chunk_checksum_at(comp, tick);
                if (ca != cb) {
                    rep.diverged = true;
                    rep.tick = tick;
                    rep.component_chunk = a.component_name_of(comp);
                    return rep;
                }
            }
        }
        return rep;
    }

    // Trace-back: halla el primer write divergente y sigue sus inputs hacia
    // atrás para construir la cadena causal mínima (superficie → raíz).
    CausalChain trace(const Replica& a, const Replica& b) const {
        CausalChain chain;
        DivergenceReport rep = find_first_divergence(a, b);
        if (!rep.diverged) return chain;

        chain.found = true;
        chain.first_divergent_tick = rep.tick;

        // 1) Escribes divergentes: mismo (tick, entidad, componente) con
        //    valor final o input distinto entre réplicas.
        std::vector<const WriteRecord*> divergent;
        for (size_t i = 0; i < a.write_count(); ++i) {
            const WriteRecord& wa = a.log()[i];
            const WriteRecord* wb = find_write(b, wa.tick, wa.entity, wa.component_name);
            if (!wb) continue;
            if (wa.new_value != wb->new_value || wa.input_value != wb->input_value) {
                divergent.push_back(&wa);
            }
        }
        if (divergent.empty()) return chain;

        // 2) Caminar hacia atrás desde la DIVERGENCIA MÁS SUPERFICIAL
        //    (mayor tick) siguiendo los inputs hacia la causa raíz.
        const WriteRecord* surface = divergent[0];
        for (auto* w : divergent) if (w->tick > surface->tick) surface = w;

        std::vector<CausalLink> links;
        const WriteRecord* cur = surface;
        int guard = 0;
        while (cur && guard++ < 64) {
            const WriteRecord* wb = find_write(b, cur->tick, cur->entity, cur->component_name);
            CausalLink link;
            link.tick = cur->tick;
            link.system = cur->system_name;
            link.component = cur->component_name;
            link.entity_desc = "entity#" + std::to_string(cur->entity);
            link.input_read = cur->input_name;
            link.input_local = cur->input_value;
            link.local_value = cur->new_value;
            link.remote_value = wb ? wb->new_value : cur->new_value;
            link.value_diverges = cur->new_value != link.remote_value;
            link.input_remote = wb ? wb->input_value : cur->input_value;
            links.push_back(link);

            // ¿El input de este write difiere? La causa está ARRIBA: quien
            // escribió input_component a un tick anterior y también diverge.
            const WriteRecord* next = nullptr;
            if (!cur->input_name.empty() && cur->input_value != link.input_remote) {
                const WriteRecord* candidate = nullptr;
                for (size_t k = 0; k < a.write_count(); ++k) {
                    const WriteRecord& wb2 = a.log()[k];
                    if (wb2.tick < cur->tick && wb2.entity == cur->entity &&
                        wb2.component_name == cur->input_name) {
                        candidate = &a.log()[k]; // última escritura previa
                    }
                }
                if (candidate) {
                    const WriteRecord* cwb = find_write(b, candidate->tick,
                                                        candidate->entity, candidate->component_name);
                    bool c_diverges = cwb && (candidate->new_value != cwb->new_value);
                    if (c_diverges) next = candidate;
                }
            }
            cur = next;
        }
        chain.links = links;

        if (!links.empty()) {
            chain.root_cause = links.back().system + " @ tick " + std::to_string(links.back().tick);
        }
        return chain;
    }

private:
    static const WriteRecord* find_write(const Replica& r, uint64_t tick,
                                         uint32_t entity, const std::string& comp) {
        for (const auto& w : r.log()) {
            if (w.tick == tick && w.entity == entity && w.component_name == comp) return &w;
        }
        return nullptr;
    }

    static uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ULL; }
        return h;
    }
};

} // namespace div
} // namespace fluxdb
