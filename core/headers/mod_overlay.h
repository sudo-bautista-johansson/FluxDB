#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #25: Data-Oriented Mod Overlays
//  (Phase 5 - Frontier / Modding)
// ─────────────────────────────────────────────────────────────
// Mods declarados en datos (archetype/component overlays) se resuelven
// contra un manifest en carga, se les asigna un namespace de IDs de
// componente ESTABLE y sin colisiones, y se fusionan en el grafo de
// arquetipos base. Los sistemas de mod comparten el scheduler del core.
// El schema resultante es hashable → compat client/server.

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>

namespace fluxdb {
namespace mod {

// Tipo de un campo de overlay.
enum class FieldKind : uint8_t { FLOAT, FLOAT3, UINT, BOOL };

struct OverlayField {
    std::string name;
    FieldKind kind = FieldKind::FLOAT;
    size_t byte_size() const {
        switch (kind) {
            case FieldKind::FLOAT:  return 4;
            case FieldKind::FLOAT3: return 12;
            case FieldKind::UINT:   return 4;
            case FieldKind::BOOL:   return 1;
        }
        return 4;
    }
};

// Espacio reservado para IDs de componente de mods (evita colisión con
// el core, que usa 0..N). Un manifest resuelve nombre → ID estable.
static constexpr uint16_t kModIdBase = 4096;

// Una declaración de overlay: añade campos a un componente existente
// (patch) o define un componente nuevo (fields_).
struct OverlayComponent {
    std::string name;
    bool is_patch = false;      // true → extiende un componente core
    std::string base_name;      // si is_patch
    std::vector<OverlayField> fields;
    uint16_t resolved_id = 0;   // rellenado por el registry al resolver
};

// Acceso declarado a componentes para un sistema de mod.
struct OverlayAccess {
    std::string component;
    bool read = false;
    bool write = false;
};

// Un sistema de mod: se registra igual que un sistema core.
struct OverlaySystem {
    std::string name;
    std::vector<OverlayAccess> access;
    std::function<void()> run; // cuerpo (compilado nativo o script)
};

// Resultado de resolución de namespace.
struct ResolvedMod {
    std::string mod_id;
    std::vector<OverlayComponent> components;
    uint64_t schema_hash = 0;
};

// Registry de mods: resuelve namespaces de IDs estables y fusiona
// overlays en el schema global (simulando el grafo de arquetipos).
class ModRegistry {
public:
    // Registra un componente core conocido (IDs 0..N-1) para que los mods
    // puedan parchearlo. Devuelve el ID asignado.
    uint16_t register_core_component(const std::string& name) {
        if (auto it = core_ids_.find(name); it != core_ids_.end()) return it->second;
        uint16_t id = static_cast<uint16_t>(next_core_id_++);
        core_ids_[name] = id;
        component_owners_[id] = "core";
        return id;
    }

    // Resuelve un manifest de mod: asigna IDs estables (kModIdBase + idx),
    // valida que los patches apunten a componentes core, y computa un
    // schema_hash para compatibilidad client/server.
    // Devuelve false si el manifest es inválido (patch a desconocido,
    // nombre duplicado en el mod).
    bool resolve(ResolvedMod& out) {
        if (resolved_.count(out.mod_id)) return false; // idempotencia
        uint64_t h = 1469598103934665603ULL; // FNV-1a base

        std::unordered_map<std::string, uint16_t> local;
        for (auto& comp : out.components) {
            if (local.count(comp.name)) return false; // duplicado interno
            if (comp.is_patch) {
                if (!core_ids_.count(comp.base_name)) return false; // patch a core inexistente
                comp.resolved_id = core_ids_[comp.base_name];
            } else {
                comp.resolved_id = static_cast<uint16_t>(kModIdBase + next_mod_id_);
                // No colisionar entre mods distintos.
                while (component_owners_.count(comp.resolved_id)) {
                    comp.resolved_id = static_cast<uint16_t>(kModIdBase + next_mod_id_ + 1);
                }
                next_mod_id_++;
                component_owners_[comp.resolved_id] = out.mod_id;
            }
            local[comp.name] = comp.resolved_id;
            h = fnv(h, comp.name);
            h = fnv(h, static_cast<uint8_t>(comp.resolved_id));
            h = fnv(h, static_cast<uint8_t>(comp.is_patch ? 1 : 0));
            for (const auto& f : comp.fields) {
                h = fnv(h, f.name);
                h = fnv(h, static_cast<uint8_t>(f.kind));
            }
        }
        out.schema_hash = h;
        resolved_[out.mod_id] = out.schema_hash;
        return true;
    }

    // ¿El componente de mod está visible para otro mod/sistema?
    bool owner_of(uint16_t id) const {
        return component_owners_.count(id) != 0;
    }

    std::string owner_of_component(uint16_t id) const {
        auto it = component_owners_.find(id);
        return it == component_owners_.end() ? std::string() : it->second;
    }

    // Hash global de todos los schemas cargados (para checksum MP).
    uint64_t global_schema_hash() const {
        uint64_t h = 1469598103934665603ULL;
        // Orden canónico: iterar los resolved en orden de carga.
        for (const auto& kv : resolved_) {
            h = fnv(h, kv.first);
            h ^= kv.second * 0x9E3779B97F4A7C15ULL;
        }
        return h;
    }

    size_t mod_count() const { return resolved_.size(); }

private:
    static uint64_t fnv(uint64_t h, const std::string& s) {
        for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ULL; }
        return h;
    }
    static uint64_t fnv(uint64_t h, uint8_t b) {
        h ^= b; h *= 1099511628211ULL;
        return h;
    }

    std::unordered_map<std::string, uint16_t> core_ids_;
    std::unordered_map<uint16_t, std::string> component_owners_;
    std::unordered_map<std::string, uint64_t> resolved_;
    uint32_t next_core_id_ = 0;
    uint32_t next_mod_id_ = 0;
};

} // namespace mod
} // namespace fluxdb
