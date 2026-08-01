// FluxDB — Feature #14: Native Blackboard & Utility Scoring Storage
// Blackboards densos por agente + evaluación batch de curvas de utilidad.
#include "../core/headers/ecs.h"
#include "../core/headers/blackboard.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::ai;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_blackboard_keys_and_values() {
    std::cout << "1. Claves tipadas y valores...\n";
    BlackboardKey health("health");
    BlackboardKey health2("health");
    BlackboardKey distance("distance");
    CHECK(health == health2);
    CHECK(!(health == distance));
    CHECK(health.id != 0);

    Blackboard bb;
    CHECK(bb.count == 0);
    bb.set(health.id, BlackboardValue::make_float(75.0f));
    bb.set(distance.id, BlackboardValue::make_int(120));
    bb.set(health.id, BlackboardValue::make_float(40.0f)); // overwrite
    CHECK(bb.count == 2);

    const BlackboardValue* h = bb.get(health.id);
    CHECK(h != nullptr);
    CHECK(std::fabs(h->f - 40.0f) < 1e-5f);
    CHECK(h->type == BlackboardValue::Type::FLOAT);

    const BlackboardValue* d = bb.get(distance.id);
    CHECK(d != nullptr && d->type == BlackboardValue::Type::INT);
    CHECK(d->i == 120);

    CHECK(bb.get(999999u) == nullptr);
}

static void test_utility_curve_evaluation() {
    std::cout << "2. Curvas de utilidad (LERP)...\n";
    UtilityCurve curve;
    curve.points.push_back({0.0f, 0.0f});
    curve.points.push_back({50.0f, 0.5f});
    curve.points.push_back({100.0f, 1.0f});

    CHECK(std::fabs(curve.evaluate(0.0f)) < 1e-5f);
    CHECK(std::fabs(curve.evaluate(100.0f) - 1.0f) < 1e-5f);
    CHECK(std::fabs(curve.evaluate(25.0f) - 0.25f) < 1e-5f);  // lerp medio
    CHECK(std::fabs(curve.evaluate(75.0f) - 0.75f) < 1e-5f);
    CHECK(std::fabs(curve.evaluate(-10.0f)) < 1e-5f);   // clamp bajo
    CHECK(std::fabs(curve.evaluate(200.0f) - 1.0f) < 1e-5f); // clamp alto

    UtilityCurve empty;
    CHECK(std::fabs(empty.evaluate(5.0f)) < 1e-5f);
}

static void test_scorer_single_and_batch() {
    std::cout << "3. UtilityScorer batch...\n";
    BlackboardKey health("health");
    BlackboardKey ammo("ammo");

    UtilityCurve health_curve;
    health_curve.points.push_back({0.0f, 0.0f});
    health_curve.points.push_back({100.0f, 1.0f});
    UtilityCurve ammo_curve;
    ammo_curve.points.push_back({0.0f, 1.0f});
    ammo_curve.points.push_back({30.0f, 0.0f});

    UtilityScorer scorer;
    scorer.add_curve(health.id, health_curve, 1.0f);
    scorer.add_curve(ammo.id, ammo_curve, 0.5f);
    CHECK(scorer.curve_count() == 2);

    // Agente sano con poca munición → salud 1.0 + ammo(0→1.0)*0.5 = 1.5
    Blackboard a1;
    a1.set(health.id, BlackboardValue::make_float(100.0f));
    a1.set(ammo.id, BlackboardValue::make_int(0));
    float s1 = scorer.score(a1);
    CHECK(std::fabs(s1 - 1.5f) < 1e-5f);

    // Agente herido (20 hp) con munición llena (30) → 0.2 + 0.0*0.5 = 0.2
    Blackboard a2;
    a2.set(health.id, BlackboardValue::make_float(20.0f));
    a2.set(ammo.id, BlackboardValue::make_int(30));
    CHECK(std::fabs(scorer.score(a2) - 0.2f) < 1e-5f);

    // Agente sin blackboard de ammo → solo salud (0.5) * 1.0
    Blackboard a3;
    a3.set(health.id, BlackboardValue::make_float(50.0f));
    CHECK(std::fabs(scorer.score(a3) - 0.5f) < 1e-5f);

    // Batch: 3 agentes.
    std::vector<Blackboard> boards = {a1, a2, a3};
    std::vector<float> out;
    scorer.score_batch(boards, out);
    CHECK(out.size() == 3);
    CHECK(std::fabs(out[0] - 1.5f) < 1e-5f);
    CHECK(std::fabs(out[1] - 0.2f) < 1e-5f);
    CHECK(std::fabs(out[2] - 0.5f) < 1e-5f);
}

static void test_blackboard_as_ecs_component() {
    std::cout << "4. Blackboard como componente ECS denso...\n";
    auto store = std::make_shared<ComponentStore>();
    World w(store);

    ComponentID bb_id = store->register_component("Blackboard", sizeof(Blackboard));

    Entity e1 = w.spawn();
    Entity e2 = w.spawn();

    Blackboard bb1_data, bb2_data;
    w.add_component(e1, bb_id, &bb1_data);
    w.add_component(e2, bb_id, &bb2_data);

    // Re-leer y mutar el blackboard del agente 1.
    BlackboardKey target("target");
    size_t out_size = 0;
    const void* ptr = w.get_entity_component_data(e1, bb_id, out_size);
    CHECK(ptr != nullptr);
    CHECK(out_size == sizeof(Blackboard));
    Blackboard* bb1 = const_cast<Blackboard*>(static_cast<const Blackboard*>(ptr));
    bb1->set(target.id, BlackboardValue::make_bool(true));

    // El agente 2 no tiene la clave.
    out_size = 0;
    const void* ptr2 = w.get_entity_component_data(e2, bb_id, out_size);
    CHECK(ptr2 != nullptr);
    const Blackboard* bb2 = static_cast<const Blackboard*>(ptr2);
    CHECK(bb2->get(target.id) == nullptr);

    // El agente 1 sí la tiene.
    CHECK(bb1->get(target.id) != nullptr);
}

int main() {
    std::cout << "--- Starting FluxDB Blackboard & Utility Scoring Test (#14) ---\n";
    test_blackboard_keys_and_values();
    test_utility_curve_evaluation();
    test_scorer_single_and_batch();
    test_blackboard_as_ecs_component();
    std::cout << "--- BLACKBOARD & UTILITY TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}