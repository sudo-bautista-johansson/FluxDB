// FluxDB — Feature #30: Hot-Reload Component Schemas Without World Reset
// Migración in-place del layout de un componente (stride) sin teardown,
// mapeando viejos offsets a nuevos y rellenando campos nuevos con default.
#include "../core/headers/ecs.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

// Viejo layout: { float x, y, z; } (12 bytes)
struct OldPos { float x, y, z; };
// Nuevo layout: { float x, y, z; float weight; } (16 bytes) — se añade weight
// al final (offset 12).

int main() {
    std::cout << "--- Starting FluxDB Hot-Reload Schemas Test (#30) ---\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store);

    ComponentID pos_id = store->register_component("Position", sizeof(OldPos));
    CHECK(store->get_info(pos_id).size == sizeof(OldPos));

    // 5 entidades con la posición vieja.
    Entity ents[5];
    for (int i = 0; i < 5; ++i) {
        ents[i] = world.spawn();
        OldPos p{static_cast<float>(i), 0, 0};
        world.add_component(ents[i], pos_id, &p);
    }

    // Verificar layout actual.
    size_t sz = 0;
    const float* p0 = static_cast<const float*>(world.get_entity_component_data(ents[0], pos_id, sz));
    CHECK(p0 != nullptr);
    CHECK(sz == sizeof(OldPos));
    CHECK(std::fabs(p0[0] - 0.0f) < 1e-5f);

    // HOT-RELOAD: nuevo tamaño 16, field map mapea x/y/z igual (offsets 0..12).
    Archetype::FieldMap map[3] = {
        {0, 0, 4},   // x
        {4, 4, 4},   // y
        {8, 8, 4},   // z
    };
    size_t migrated = world.hot_reload_component(pos_id, 16, map, 3, 0);
    CHECK(migrated == 5);

    // El store ahora reporta el nuevo tamaño.
    CHECK(store->get_info(pos_id).size == 16);

    // Los datos de cada entidad quedaron re-mapeados y el campo nuevo = default.
    for (int i = 0; i < 5; ++i) {
        sz = 0;
        const uint8_t* data = static_cast<const uint8_t*>(
            world.get_entity_component_data(ents[i], pos_id, sz));
        CHECK(sz == 16);
        float x, y, z, w;
        std::memcpy(&x, data, 4);
        std::memcpy(&y, data + 4, 4);
        std::memcpy(&z, data + 8, 4);
        std::memcpy(&w, data + 12, 4);
        CHECK(std::fabs(x - static_cast<float>(i)) < 1e-5f);
        CHECK(std::fabs(y) < 1e-5f);
        CHECK(std::fabs(z) < 1e-5f);
        CHECK(std::fabs(w) < 1e-5f); // campo nuevo = default 0
    }

    // Escrituras posteriores usan el nuevo stride.
    struct NewPos { float x, y, z; float weight; };
    NewPos np{100.0f, 200.0f, 300.0f, 7.0f};
    world.add_component(ents[0], pos_id, &np);
    sz = 0;
    const float* nd = static_cast<const float*>(world.get_entity_component_data(ents[0], pos_id, sz));
    CHECK(sz == 16);
    CHECK(std::fabs(nd[0] - 100.0f) < 1e-5f);
    CHECK(std::fabs(nd[3] - 7.0f) < 1e-5f);

    // Entidades nuevas tras el reload usan el layout nuevo también.
    Entity e6 = world.spawn();
    world.add_component(e6, pos_id, &np);
    sz = 0;
    const void* d6 = world.get_entity_component_data(e6, pos_id, sz);
    CHECK(sz == 16);

    // hot_reload de tamaño idéntico → 0 migraciones (no-op).
    CHECK(world.hot_reload_component(pos_id, 16, map, 3, 0) == 0);

    std::cout << "--- HOT-RELOAD SCHEMAS TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}