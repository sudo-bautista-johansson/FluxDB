#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #24: Live Schema Evolution (Hot State Migration)
//  (Phase 4 - Developer Experience)
// ─────────────────────────────────────────────────────────────
// Cada definición de componente lleva una VERSIÓN y una tabla de
// migración declarativa (campo renombrado/añadido/quitado/cambiado de
// tipo, con regla de default/conversión). Al cargar, FluxDB recorre el
// delta de versiones y aplica las migraciones por fila. Las migraciones
// son funciones puras del dato viejo → paralelizables por chunk.
// En caliente reutiliza Archetype::migrate_component_layout (#30).

#include "ecs.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

namespace fluxdb {
namespace schema {

using fluxdb::ecs::World;
using fluxdb::ecs::ComponentID;
using fluxdb::ecs::Archetype;

// Tipos de operación de migración por campo.
enum class FieldOp : uint8_t {
    COPY,        // copiar bytes de old_offset a new_offset (longitud dada)
    SET_DEFAULT, // rellenar new_offset con bytes de default_data (campo nuevo)
};

// Una regla de migración declarativa.
struct MigrationRule {
    uint32_t from_version = 0;
    uint32_t to_version = 0;
    size_t old_size = 0;
    size_t new_size = 0;
    // Mapa de campos: cada regla transforma un layout viejo → nuevo.
    std::vector<Archetype::FieldMap> field_map;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> defaults; // (offset, bytes)

    // Arma el field_map completo a partir de las ops declarativas.
    void build_field_map() {
        // (las ops COPY ya vienen como FieldMap; defaults se aplican aparte)
    }
};

// Registro de versiones por componente + cadena de migraciones.
class SchemaRegistry {
public:
    // Registra un componente con su versión de schema actual.
    void register_component_version(const std::string& name, uint32_t version) {
        versions_[name] = version;
    }

    uint32_t version_of(const std::string& name) const {
        auto it = versions_.find(name);
        return it == versions_.end() ? 0 : it->second;
    }

    // Declara una regla de migración (from → to) para un componente.
    void add_migration(const std::string& name, MigrationRule rule) {
        auto& chain = migrations_[name];
        chain.push_back(std::move(rule));
    }

    bool has_migrations(const std::string& name) const {
        return migrations_.find(name) != migrations_.end();
    }

    // Camino de migración: reglas que llevan desde `from` hasta `to`.
    // Sigue la cadena hacia adelante mientras haya reglas contiguas.
    std::vector<const MigrationRule*> path(const std::string& name,
                                           uint32_t from, uint32_t to) const {
        std::vector<const MigrationRule*> out;
        auto it = migrations_.find(name);
        if (it == migrations_.end()) return out;
        uint32_t current = from;
        // Buscar reglas que empiecen en `current` hasta llegar a `to`.
        bool advanced = true;
        while (current < to && advanced) {
            advanced = false;
            for (const auto& r : it->second) {
                if (r.from_version == current && r.to_version <= to) {
                    out.push_back(&r);
                    current = r.to_version;
                    advanced = true;
                    break;
                }
            }
        }
        return out;
    }

private:
    std::unordered_map<std::string, uint32_t> versions_;
    std::unordered_map<std::string, std::vector<MigrationRule>> migrations_;
};

// Aplica migraciones de schema a un World en vivo (o recién cargado).
// Walk per-chunk: cada regla usa migrate_component_layout (#30).
class SchemaMigrator {
public:
    SchemaMigrator(SchemaRegistry& registry, World& world)
        : registry_(registry), world_(world) {}

    // Migra `comp` desde su versión registrada en el store a `target_version`.
    // Devuelve el número de filas migradas.
    size_t migrate(ComponentID comp, const std::string& name, uint32_t target_version) {
        // La versión actual la llevamos en el registry (no hay versiones
        // multi-mundo en este demo; la app mantiene el schema en el registry).
        uint32_t from = registry_.version_of(name);
        if (from >= target_version) return 0;

        auto path = registry_.path(name, from, target_version);
        size_t migrated = 0;
        for (const MigrationRule* r : path) {
            // Aplica el field_map declarado + defaults.
            size_t m = world_.hot_reload_component(
                comp, r->new_size, r->field_map.data(), r->field_map.size(), 0);
            if (m == 0 && !r->field_map.empty()) return 0; // algo falló
            migrated += m;

            // Aplica defaults (campos nuevos) fila por fila.
            apply_defaults(comp, r);
            // El registry ahora sabe que estamos en la nueva versión.
            registry_.register_component_version(name, r->to_version);
        }
        return migrated;
    }

private:
    // Rellena los campos nuevos (defaults) en cada entidad con el componente.
    void apply_defaults(ComponentID comp, const MigrationRule* r) {
        for (const auto& [offset, bytes] : r->defaults) {
            for (auto& [sig, up] : world_.get_archetypes()) {
                if (!up->has_component(comp)) continue;
                const fluxdb::ecs::Entity* ents = up->get_entities_ptr();
                if (!ents) continue;
                for (size_t row = 0; row < up->get_entity_count(); ++row) {
                    uint8_t* data = static_cast<uint8_t*>(up->get_component_data(row, comp));
                    if (data) {
                        std::memcpy(data + offset, bytes.data(), bytes.size());
                    }
                }
            }
        }
    }

    SchemaRegistry& registry_;
    World& world_;
};

} // namespace schema
} // namespace fluxdb
