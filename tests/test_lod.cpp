#include "../core/headers/ecs.h"
#include "../core/headers/lod.h"
#include "../core/headers/pubsub.h"
#include "../core/headers/delta_set.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::query;
using namespace fluxdb::lod;

struct Position {
    float x, y, z;
};

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void set_pos(World& world, Entity e, ComponentID pos_id, float x, float y, float z) {
    Position p{x, y, z};
    world.add_component(e, pos_id, &p);
}

static void test_tier_selection() {
    std::cout << "1. Selección de tier por distancia...\n";

    LodManager lod;
    std::vector<LODRule> rules = {
        {20.0f, 1, 0.0f},    // FULL: 0-20m
        {100.0f, 2, 0.5f},   // REDUCED: 20-100m
        {500.0f, 4, 1.0f},   // MINIMAL: 100-500m
    };
    lod.set_component_rules(3, rules);

    CHECK(lod.has_rules(3));
    CHECK(!lod.has_rules(4));

    CHECK(lod.tier_for(3, 5.0f) == Tier::FULL);
    CHECK(lod.tier_for(3, 20.0f) == Tier::FULL);   // límite inclusive
    CHECK(lod.tier_for(3, 50.0f) == Tier::REDUCED);
    CHECK(lod.tier_for(3, 100.0f) == Tier::REDUCED);
    CHECK(lod.tier_for(3, 300.0f) == Tier::MINIMAL);
    CHECK(lod.tier_for(3, 600.0f) == Tier::NONE);  // fuera del último rango

    // Componente sin reglas: siempre FULL
    CHECK(lod.tier_for(9, 100000.0f) == Tier::FULL);
    CHECK(lod.rule_for(9, 100000.0f).every_n_ticks == 1);

    CHECK(lod.rule_for(3, 50.0f).step == 0.5f);
    CHECK(lod.rule_for(3, 300.0f).every_n_ticks == 4);
}

static void test_quantization() {
    std::cout << "2. Cuantización float32 por tier...\n";

    LodManager lod;
    lod.set_component_rules(1, {{100.0f, 1, 0.0f}, {1000.0f, 1, 0.5f}, {5000.0f, 1, 1.0f}});

    float data[] = {1.73f, 2.4f, 3.1f};
    // FULL (dist 5): sin cuantizar
    lod.quantize(1, Tier::FULL, reinterpret_cast<uint8_t*>(data), sizeof(data));
    CHECK(std::fabs(data[0] - 1.73f) < 1e-4f);

    float reduced[] = {5.55f, 6.66f, 7.77f};
    lod.quantize(1, Tier::REDUCED, reinterpret_cast<uint8_t*>(reduced), sizeof(reduced));
    CHECK(std::fabs(reduced[0] - 5.5f) < 1e-4f); // 5.55 -> 5.5
    CHECK(std::fabs(reduced[1] - 6.5f) < 1e-4f); // 6.66 -> 6.5
    CHECK(std::fabs(reduced[2] - 8.0f) < 1e-4f); // 7.77 -> 8.0

    float minimal[] = {9.9f, 8.8f, 7.7f};
    lod.quantize(1, Tier::MINIMAL, reinterpret_cast<uint8_t*>(minimal), sizeof(minimal));
    CHECK(minimal[0] == 10.0f);
    CHECK(minimal[1] == 9.0f);
    CHECK(minimal[2] == 8.0f);

    // Datos no-float (size % 4 != 0): no se tocan
    uint8_t raw[] = {1, 2, 3};
    lod.quantize(1, Tier::MINIMAL, raw, sizeof(raw));
    CHECK(raw[0] == 1 && raw[1] == 2 && raw[2] == 3);
}

static void test_rate_limiting() {
    std::cout << "3. Frecuencia de actualización por tier...\n";

    LodManager lod;
    lod.set_component_rules(2, {{100.0f, 1, 0.0f}, {1000.0f, 2, 0.0f}}); // REDUCED: cada 2 ticks

    // Tier REDUCED, entity 7: pendiente desde tick 1
    CHECK(lod.should_update(2, 7, Tier::REDUCED, 1, 1));  // nunca enviado: sí
    lod.mark_sent(2, 7, Tier::REDUCED, 1);
    CHECK(!lod.should_update(2, 7, Tier::REDUCED, 1, 2)); // sin cambios nuevos
    CHECK(!lod.should_update(2, 7, Tier::REDUCED, 2, 2)); // cambio nuevo pero 2-1 < 2
    CHECK(lod.should_update(2, 7, Tier::REDUCED, 3, 3));  // cambio nuevo y 3-1 >= 2
    lod.mark_sent(2, 7, Tier::REDUCED, 3);

    // FULL (every 1): siempre due si hay cambios
    CHECK(lod.should_update(2, 7, Tier::FULL, 4, 4));
    lod.mark_sent(2, 7, Tier::FULL, 4);
    CHECK(lod.should_update(2, 7, Tier::FULL, 5, 5));

    // Sin cambios pendientes: nunca envía
    CHECK(!lod.should_update(2, 7, Tier::FULL, 0, 9));
}

static void test_subscriber_delta_pipeline() {
    std::cout << "4. Pipeline ECS → delta del suscriptor (relevancia + LOD)...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    ComponentID data_id = store->register_component("Stats", sizeof(Position));
    world.set_position_component_id(pos_id);

    // Stats: FULL (0-20m, sin cuantizar) / REDUCED (20-100m, paso 0.5) /
    // MINIMAL (100-500m, paso 1.0) / NONE (500m+). Position no tiene reglas
    // → siempre FULL (identidad).
    world.set_component_lod(data_id, {
        {20.0f, 1, 0.0f},
        {100.0f, 1, 0.5f},
        {500.0f, 1, 1.0f},
    });

    // Suscriptor con AOI esférico en el origen (r 1000)
    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 0; aoi.cy = 0; aoi.cz = 0;
    aoi.r = 1000.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    Entity near_e = world.spawn();
    set_pos(world, near_e, pos_id, 5.0f, 0.0f, 0.0f);      // FULL (dist 5)
    Entity mid_e = world.spawn();
    set_pos(world, mid_e, pos_id, 50.0f, 0.0f, 0.0f);      // REDUCED (dist 50)
    Entity far_e = world.spawn();
    set_pos(world, far_e, pos_id, 300.0f, 0.0f, 0.0f);     // MINIMAL (dist 300)
    Entity out_e = world.spawn();
    set_pos(world, out_e, pos_id, 5000.0f, 0.0f, 0.0f);    // fuera del AOI

    world.flush_interest_events(); // backfill: relevantes = near/mid/far

    // Payloads en tick 1 (Position NO se toca: sigue siendo la posición real)
    world.advance_tick();
    Position p;
    p.x = 1.73f; p.y = 2.4f; p.z = 3.1f;
    world.add_component(near_e, data_id, &p);
    p.x = 5.55f; p.y = 6.66f; p.z = 7.77f;
    world.add_component(mid_e, data_id, &p);
    p.x = 9.9f; p.y = 8.8f; p.z = 7.7f;
    world.add_component(far_e, data_id, &p);
    p.x = 1.0f; p.y = 1.0f; p.z = 1.0f;
    world.add_component(out_e, data_id, &p);

    delta::DeltaSet d = world.build_subscriber_delta(sub, 0);

    const Position* near_data = nullptr;
    const Position* mid_data = nullptr;
    const Position* far_data = nullptr;
    bool out_seen = false;
    d.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op != delta::DeltaOp::UPDATE) return;
        if (r.entity == near_e) near_data = reinterpret_cast<const Position*>(r.data.data());
        if (r.entity == mid_e) mid_data = reinterpret_cast<const Position*>(r.data.data());
        if (r.entity == far_e) far_data = reinterpret_cast<const Position*>(r.data.data());
        if (r.entity == out_e) out_seen = true;
    });

    // FULL: precisión completa
    CHECK(near_data != nullptr);
    CHECK(std::fabs(near_data->x - 1.73f) < 1e-4f);
    CHECK(std::fabs(near_data->y - 2.4f) < 1e-4f);

    // REDUCED: paso 0.5
    CHECK(mid_data != nullptr);
    CHECK(std::fabs(mid_data->x - 5.5f) < 1e-4f);
    CHECK(std::fabs(mid_data->y - 6.5f) < 1e-4f);
    CHECK(std::fabs(mid_data->z - 8.0f) < 1e-4f);

    // MINIMAL: paso 1.0
    CHECK(far_data != nullptr);
    CHECK(far_data->x == 10.0f);
    CHECK(far_data->y == 9.0f);
    CHECK(far_data->z == 8.0f);

    // Fuera del AOI (relevancia #9): nunca llega
    CHECK(!out_seen);
}

static void test_rate_in_subscriber_delta() {
    std::cout << "5. Frecuencia LOD en el pipeline del suscriptor...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    ComponentID vel_id = store->register_component("Velocity", sizeof(Position));
    world.set_position_component_id(pos_id);

    // Velocity: cada 2 ticks (REDUCED a 20-100m)
    world.set_component_lod(vel_id, {
        {20.0f, 1, 0.0f},
        {100.0f, 2, 0.0f},
    });

    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 0; aoi.cy = 0; aoi.cz = 0;
    aoi.r = 1000.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    Entity e = world.spawn();
    set_pos(world, e, pos_id, 50.0f, 0.0f, 0.0f); // REDUCED tier para Velocity
    Position v{1.0f, 2.0f, 3.0f};
    world.add_component(e, vel_id, &v);

    world.flush_interest_events();

    bool seen_tick1 = false, seen_tick2 = false, seen_tick3 = false;
    auto scan = [&](const delta::DeltaSet& d, bool& flag) {
        d.for_each_record([&](const delta::DeltaRecord& r) {
            if (r.op == delta::DeltaOp::UPDATE && r.comp_id == vel_id) flag = true;
        });
    };

    // tick 1: cambio → se envía
    world.advance_tick();
    v.x = 1.5f;
    world.add_component(e, vel_id, &v);
    scan(world.build_subscriber_delta(sub, 0), seen_tick1);
    CHECK(seen_tick1);

    // tick 2: cambio nuevo, pero REDUCED envía cada 2 ticks → NO
    world.advance_tick();
    v.x = 2.5f;
    world.add_component(e, vel_id, &v);
    scan(world.build_subscriber_delta(sub, 1), seen_tick2);
    CHECK(!seen_tick2);

    // tick 3: cambió y pasó el intervalo → se envía (con el valor nuevo)
    world.advance_tick();
    v.x = 3.5f;
    world.add_component(e, vel_id, &v);
    delta::DeltaSet d3 = world.build_subscriber_delta(sub, 2);
    scan(d3, seen_tick3);
    CHECK(seen_tick3);
    const Position* got = nullptr;
    d3.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::UPDATE && r.comp_id == vel_id) {
            got = reinterpret_cast<const Position*>(r.data.data());
        }
    });
    CHECK(got != nullptr && std::fabs(got->x - 3.5f) < 1e-4f);
}

static void test_relevancy_structural() {
    std::cout << "6. Spawn/despawn por relevancia...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    world.set_position_component_id(pos_id);

    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 0; aoi.cy = 0; aoi.cz = 0;
    aoi.r = 100.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    Entity inside = world.spawn();
    set_pos(world, inside, pos_id, 10.0f, 0.0f, 0.0f);
    Entity outside = world.spawn();
    set_pos(world, outside, pos_id, 5000.0f, 0.0f, 0.0f);
    Entity die_e = world.spawn();
    set_pos(world, die_e, pos_id, 20.0f, 0.0f, 0.0f);

    world.flush_interest_events();

    // delta0 (since 0): near y die_e son relevantes → SPAWN. outside NO.
    delta::DeltaSet d0 = world.build_subscriber_delta(sub, 0);
    bool spawned_inside0 = false, spawned_die0 = false, spawned_outside0 = false;
    d0.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::SPAWN) {
            if (r.entity == inside) spawned_inside0 = true;
            if (r.entity == die_e) spawned_die0 = true;
            if (r.entity == outside) spawned_outside0 = true;
        }
    });
    CHECK(spawned_inside0);
    CHECK(spawned_die0);
    CHECK(!spawned_outside0);

    // tick 1: outside entra al AOI → su SPAWN SÍ se emite (el suscriptor no
    // la conocía); die_e se despawna en el mundo → DESPAWN (ya la conocía).
    world.advance_tick();
    set_pos(world, outside, pos_id, 20.0f, 0.0f, 0.0f);
    world.despawn(die_e);
    world.flush_interest_events();

    delta::DeltaSet d = world.build_subscriber_delta(sub, 1);
    bool spawned_outside = false, despawned_die = false, spawned_inside_again = false;
    d.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::SPAWN) {
            if (r.entity == outside) spawned_outside = true;
            if (r.entity == inside) spawned_inside_again = true;
        }
        if (r.op == delta::DeltaOp::DESPAWN && r.entity == die_e) despawned_die = true;
    });
    CHECK(spawned_outside);
    CHECK(despawned_die);
    CHECK(!spawned_inside_again); // ya la conocía: sin re-spawn

    // tick 2: inside sale del AOI → DESPAWN emitido (cleanup del cliente).
    // La salida NO se re-emite en deltas posteriores (known ya la descartó).
    world.advance_tick();
    set_pos(world, inside, pos_id, 5000.0f, 0.0f, 0.0f);
    world.flush_interest_events();

    delta::DeltaSet d2 = world.build_subscriber_delta(sub, 1);
    int despawns_inside = 0;
    d2.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::DESPAWN && r.entity == inside) ++despawns_inside;
    });
    CHECK(despawns_inside == 1);

    delta::DeltaSet d3 = world.build_subscriber_delta(sub, 2);
    int despawns_again = 0;
    d3.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::DESPAWN && r.entity == inside) ++despawns_again;
    });
    CHECK(despawns_again == 0);
}

int main() {
    std::cout << "--- Starting FluxDB Component LOD Test (#10) ---\n";

    test_tier_selection();
    test_quantization();
    test_rate_limiting();
    test_subscriber_delta_pipeline();
    test_rate_in_subscriber_delta();
    test_relevancy_structural();

    std::cout << "--- LOD TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
