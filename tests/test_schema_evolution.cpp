// FluxDB — Feature #24: Live Schema Evolution (Hot State Migration)
// Migración declarativa por versiones: campo añadido/remapeado con defaults.
#include "../core/headers/ecs.h"
#include "../core/headers/schema_evolution.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::schema;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

// v1: { float hp; }                (4 bytes)
// v2: { float hp; float max_hp; }  (8 bytes) — se añade max_hp = 100 default
struct V1 { float hp; };
struct V2 { float hp; float max_hp; };

int main() {
    std::cout << "--- Starting FluxDB Live Schema Evolution Test (#24) ---\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store);

    ComponentID comp = store->register_component("Health", sizeof(V1));

    // 10 entidades con schema v1.
    Entity ents[10];
    for (int i = 0; i < 10; ++i) {
        ents[i] = world.spawn();
        V1 v{50.0f + i};
        world.add_component(ents[i], comp, &v);
    }

    SchemaRegistry registry;
    registry.register_component_version("Health", 1);

    // Regla de migración v1 → v2: hp se copia (0→0), max_hp nueva con default.
    MigrationRule rule;
    rule.from_version = 1;
    rule.to_version = 2;
    rule.old_size = sizeof(V1);
    rule.new_size = sizeof(V2);
    rule.field_map.push_back({0, 0, sizeof(float)}); // hp: old 0 → new 0
    // default para max_hp en offset 4: float 100.0f
    float default_max = 100.0f;
    rule.defaults.push_back({offsetof(V2, max_hp),
                             std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&default_max),
                                                  reinterpret_cast<uint8_t*>(&default_max) + sizeof(float))});
    registry.add_migration("Health", std::move(rule));

    // Confirmar que no hay camino hacia atrás ni saltos.
    CHECK(registry.version_of("Health") == 1);
    CHECK(registry.path("Health", 1, 1).empty());
    CHECK(registry.has_migrations("Health"));

    // Migrar en caliente: v1 → v2.
    SchemaMigrator migrator(registry, world);
    size_t migrated = migrator.migrate(comp, "Health", 2);
    CHECK(migrated == 10);
    CHECK(registry.version_of("Health") == 2);
    CHECK(store->get_info(comp).size == sizeof(V2));

    // Verificar datos migrados: hp conservado, max_hp = default.
    for (int i = 0; i < 10; ++i) {
        size_t sz = 0;
        const V2* d = static_cast<const V2*>(world.get_entity_component_data(
            ents[i], comp, sz));
        CHECK(d != nullptr);
        CHECK(sz == sizeof(V2));
        if (!(std::fabs(d->hp - (50.0f + i)) < 1e-5f)) {
            std::cout << "  mismatch i=" << i << " entity=" << ents[i]
                      << " hp=" << d->hp << " max_hp=" << d->max_hp << "\n";
        }
        CHECK(std::fabs(d->hp - (50.0f + i)) < 1e-5f);
        CHECK(std::fabs(d->max_hp - 100.0f) < 1e-5f);
    }

    // Migrar a la misma versión → no-op.
    CHECK(migrator.migrate(comp, "Health", 2) == 0);

    // Registro de otra versión sin regla → sin camino.
    registry.register_component_version("Armor", 5);
    CHECK(registry.version_of("Armor") == 5);
    CHECK(registry.path("Armor", 5, 9).empty());

    std::cout << "--- LIVE SCHEMA EVOLUTION TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}