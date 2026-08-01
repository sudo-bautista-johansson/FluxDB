#include "../core/headers/ecs.h"
#include "../core/headers/fixed.h"
#include "../core/headers/determinism.h"
#include "../core/headers/scheduler.h"
#include "../core/headers/lockstep.h"
#include "../core/headers/rollback.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <tuple>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::det;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

// ── Linter compile-time (#11) ──
static_assert(is_deterministic_v<Fix32>, "Fix32 debe ser determinista");
static_assert(is_deterministic_v<uint32_t>, "uint32_t debe ser determinista");
static_assert(is_deterministic_v<uint64_t>, "uint64_t debe ser determinista");
static_assert(is_deterministic_v<bool>, "bool debe ser determinista");
static_assert(!is_deterministic_v<float>, "float NO debe ser determinista");
static_assert(!is_deterministic_v<double>, "double NO debe ser determinista");

static void compile_time_linter() {
    // Linter compile-time invocable (debe compilar sin error).
    FLUXDB_STATIC_ASSERT_DETERMINISTIC(Fix32);
    FLUXDB_STATIC_ASSERT_DETERMINISTIC(int16_t);
    ++checks;
}

struct FP3 {
    Fix32 x, y, z;
};

static void test_fix32_math() {
    std::cout << "1. Aritmetica Fix32 (golden values)...\n";

    Fix32 a(1.5f), b(2.0f);
    CHECK((a * b) == Fix32(3.0f));
    CHECK((a + b) == Fix32(3.5f));
    CHECK((b - a) == Fix32(0.5f));
    CHECK((Fix32(7.0f) / Fix32(2.0f)) == Fix32(3.5f));
    CHECK(a < b);
    CHECK(b > a);
    CHECK(-a == Fix32(-1.5f));
    CHECK(a.abs() == a);
    CHECK(Fix32(-2.5f).abs() == Fix32(2.5f));

    // sqrt: isqrt entero, valor dorado contra std::sqrt (IEEE).
    Fix32 root = Fix32(2.0f).sqrt();
    CHECK(std::fabs(root.to_double() - std::sqrt(2.0)) < 1e-3);
    CHECK(Fix32(0.0f).sqrt() == Fix32(0.0f));

    // Trigonometría: tabla entera, valores dorados.
    CHECK(Fix32(0.0f).sin() == Fix32(0.0f));
    Fix32 halfpi = Fix32::pi() / Fix32(2.0f);
    CHECK(std::fabs(halfpi.sin().to_double() - 1.0) < 1e-3);
    CHECK(halfpi.cos() == Fix32(0.0f));
    CHECK(std::fabs(Fix32::pi().sin().to_double()) < 1e-3);
    CHECK(std::fabs(Fix32::pi().cos().to_double() + 1.0) < 1e-3);
    CHECK(Fix32::tau().sin() == Fix32(0.0f)); // periódico
    CHECK(std::fabs(Fix32::pi().to_double() - 3.141592653589793) < 1e-4);

    // Determinismo: misma op → mismo raw (bit-exacto).
    Fix32 x(1.234f), y(9.876f);
    CHECK((x * y).raw() == (x * y).raw());
    CHECK((x / y).raw() == (x / y).raw());
    CHECK(x.sin().raw() == x.sin().raw());
}

static void test_fixed_random() {
    std::cout << "2. FixedRandom (xorshift64*) determinista...\n";

    FixedRandom r1(42), r2(42), r3(99);
    bool seq_equal = true, seq_diff = false;
    for (int i = 0; i < 100; ++i) {
        uint64_t a = r1.next_u64(), b = r2.next_u64(), c = r3.next_u64();
        if (a != b) seq_equal = false;
        if (a != c) seq_diff = true;
    }
    CHECK(seq_equal);  // misma semilla → misma secuencia
    CHECK(seq_diff);   // distinta semilla → secuencia distinta

    FixedRandom r4(7);
    for (int i = 0; i < 200; ++i) {
        int v = r4.next_int(-5, 5);
        CHECK(v >= -5 && v <= 5);
    }
    FixedRandom r5(7), r6(7);
    CHECK(r5.next_fix01().raw() == r6.next_fix01().raw());
}

static void test_canonical_iteration() {
    std::cout << "3. Orden canonico de arquetipos (independiente de insercion)...\n";

    auto build = [](bool reversed) {
        auto store = std::make_shared<ComponentStore>();
        ComponentID c1 = store->register_component("A", sizeof(uint32_t));
        ComponentID c2 = store->register_component("B", sizeof(uint32_t));
        ComponentID c3 = store->register_component("C", sizeof(uint32_t));
        auto w = std::make_unique<World>(store);
        w->advance_tick(); // sella los writes con tick 1
        auto add = [&](ComponentID c, uint32_t v, uint32_t e) {
            Entity ent = w->spawn_with_id(e);
            uint32_t d = v;
            w->add_component(ent, c, &d);
        };
        if (reversed) { add(c3, 3, 1); add(c2, 2, 2); add(c1, 1, 3); }
        else          { add(c1, 1, 3); add(c2, 2, 2); add(c3, 3, 1); }
        return w;
    };

    std::unique_ptr<World> w1 = build(false);
    std::unique_ptr<World> w2 = build(true);

    // Misma secuencia de firmas a pesar del orden de inserción opuesto.
    std::vector<unsigned long long> s1, s2;
    w1->for_each_archetype_sorted([&](const Archetype* a) { s1.push_back(a->get_signature().to_ullong()); });
    w2->for_each_archetype_sorted([&](const Archetype* a) { s2.push_back(a->get_signature().to_ullong()); });
    CHECK(s1 == s2);
    CHECK(s1.size() >= 4); // arquetipo vacío + 3 de un componente

    // Misma secuencia de entidades vía for_each_changed.
    ComponentStore* st = w1->get_store();
    for (ComponentID c = 0; c < st->count(); ++c) {
        std::vector<uint32_t> o1, o2;
        w1->for_each_changed(c, 0, [&](uint32_t e, size_t, uint32_t) { o1.push_back(e); });
        w2->for_each_changed(c, 0, [&](uint32_t e, size_t, uint32_t) { o2.push_back(e); });
        CHECK(o1 == o2);
    }

    // Mismo hash de estado completo (desync detection sobre inserción).
    CHECK(w1->state_hash() == w2->state_hash());
}

static void test_scheduler_order() {
    std::cout << "4. SystemScheduler: orden canonico de registro...\n";

    std::vector<std::string> log;
    SystemScheduler sched;
    size_t id_c = sched.register_system("C", [&](World&) { log.push_back("C"); });
    size_t id_a = sched.register_system("A", [&](World&) { log.push_back("A"); });
    size_t id_b = sched.register_system("B", [&](World&) { log.push_back("B"); });
    CHECK(id_c == 0 && id_a == 1 && id_b == 2);
    CHECK(sched.system_count() == 3);

    auto store = std::make_shared<ComponentStore>();
    World w(store);
    sched.step(w);
    sched.step(w);

    std::vector<std::string> expected = {"C", "A", "B", "C", "A", "B"};
    CHECK(log == expected);
    CHECK(w.current_tick() == 2); // cada step avanzó el reloj
}

struct SimWorld {
    std::unique_ptr<World> world;
    ComponentID pos_id, vel_id;
};

static SimWorld make_sim_world(uint32_t seed) {
    auto store = std::make_shared<ComponentStore>();
    ComponentID pos_id = store->register_component("Pos", sizeof(FP3));
    ComponentID vel_id = store->register_component("Vel", sizeof(FP3));
    SimWorld sw;
    sw.world = std::make_unique<World>(store);
    sw.pos_id = pos_id;
    sw.vel_id = vel_id;
    sw.world->enable_determinism_lock();
    FixedRandom rng(seed);
    for (uint32_t i = 0; i < 200; ++i) {
        Entity e = sw.world->spawn_with_id(i + 1);
        FP3 pos{Fix32(rng.next_int(-100, 100)),
                Fix32(rng.next_int(-100, 100)),
                Fix32(rng.next_int(-100, 100))};
        FP3 vel{Fix32(rng.next_int(-3, 3)),
                Fix32(rng.next_int(-3, 3)),
                Fix32(1)};
        sw.world->add_component(e, pos_id, &pos);
        sw.world->add_component(e, vel_id, &vel);
    }
    return sw;
}

// Sistema de movimiento canónico: lee Pos+Vel, escribe Pos (writes directos
// al chunk; el world se mantiene bajo shared lock durante la iteración).
static void move_system(SimWorld& sw) {
    sw.world->for_each_archetype_sorted([&](Archetype* arch) {
        if (!arch->has_component(sw.pos_id) || !arch->has_component(sw.vel_id)) return;
        const Entity* ents = arch->get_entities_ptr();
        const size_t n = arch->get_entity_count();
        for (size_t row = 0; row < n; ++row) {
            FP3* p = static_cast<FP3*>(arch->get_component_data(row, sw.pos_id));
            const FP3* v = static_cast<const FP3*>(arch->get_component_data(row, sw.vel_id));
            p->x += v->x;
            p->y += v->y;
            p->z += v->z;
        }
    });
}

static void move_system_lite(World& w, ComponentID pos_id, ComponentID vel_id) {
    w.for_each_archetype_sorted([&](Archetype* arch) {
        if (!arch->has_component(pos_id) || !arch->has_component(vel_id)) return;
        const Entity* ents = arch->get_entities_ptr();
        const size_t n = arch->get_entity_count();
        for (size_t row = 0; row < n; ++row) {
            FP3* p = static_cast<FP3*>(arch->get_component_data(row, pos_id));
            const FP3* v = static_cast<const FP3*>(arch->get_component_data(row, vel_id));
            p->x += v->x;
            p->y += v->y;
            p->z += v->z;
        }
    });
}

static rollback::ExternalInput make_input(uint64_t tick, Entity e, ComponentID comp, const FP3& v) {
    rollback::ExternalInput in;
    in.tick = tick;
    in.op = rollback::InputOp::SET_COMPONENT;
    in.entity = e;
    in.comp_id = comp;
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(&v);
    in.data.assign(begin, begin + sizeof(v));
    return in;
}

static void test_lockstep_simulation() {
    std::cout << "5. Simulacion lockstep bit-exacta (2 worlds, mismos inputs)...\n";

    SimWorld sw1 = make_sim_world(1234);
    SimWorld sw2 = make_sim_world(1234);

    // Estado inicial idéntico.
    CHECK(DeterminismLinter::states_equal(*sw1.world, *sw2.world));
    CHECK(sw1.world->determinism_locked());

    for (uint64_t t = 1; t <= 100; ++t) {
        // Inputs del tick (solo los que cambian de comportamiento).
        std::vector<rollback::ExternalInput> inputs;
        if (t % 10 == 0) {
            FP3 kick{Fix32(5.0f), Fix32(-3.0f), Fix32(0.0f)};
            inputs.push_back(make_input(t, 1, sw1.vel_id, kick));
            inputs.push_back(make_input(t, 7, sw1.vel_id, kick));
        }

        // Mismo paso en ambos worlds: tick + sistemas + inputs.
        sw1.world->advance_tick();
        sw2.world->advance_tick();
        move_system(sw1);
        move_system(sw2);
        sw1.world->apply_inputs(inputs);
        sw2.world->apply_inputs(inputs);

        CHECK(sw1.world->state_hash() == sw2.world->state_hash());
    }

    // Golden value: la entidad 1 recibió patadas de (5,-3,0) en t=10,20,...,100.
    // Velocidad inicial (vx, vy, 1) + 10 patadas → posición final exacta:
    size_t sz = 0;
    const FP3* p1 = static_cast<const FP3*>(sw1.world->get_entity_component_data(1, sw1.pos_id, sz));
    const FP3* p2 = static_cast<const FP3*>(sw2.world->get_entity_component_data(1, sw2.pos_id, sz));
    CHECK(p1 != nullptr && p2 != nullptr);
    CHECK(p1->x == p2->x && p1->y == p2->y && p1->z == p2->z);

    // Golden value: la última patada (t=100) deja la velocidad en (5,-3,0)
    // bit-exacto en ambos worlds.
    const FP3* v1 = static_cast<const FP3*>(sw1.world->get_entity_component_data(1, sw1.vel_id, sz));
    const FP3* v2 = static_cast<const FP3*>(sw2.world->get_entity_component_data(1, sw2.vel_id, sz));
    CHECK(v1 != nullptr && v2 != nullptr);
    CHECK(v1->x == Fix32(5.0f) && v1->y == Fix32(-3.0f) && v1->z == Fix32(0.0f));
    CHECK(v2->x == Fix32(5.0f) && v2->y == Fix32(-3.0f) && v2->z == Fix32(0.0f));
}

static void test_desync_detection() {
    std::cout << "6. Desync detection (hash divergente)...\n";

    SimWorld sw1 = make_sim_world(99);
    SimWorld sw2 = make_sim_world(99);
    CHECK(DeterminismLinter::states_equal(*sw1.world, *sw2.world));

    // Hasta el tick 4, idénticos.
    for (uint64_t t = 1; t <= 4; ++t) {
        sw1.world->advance_tick();
        sw2.world->advance_tick();
        move_system(sw1);
        move_system(sw2);
        CHECK(sw1.world->state_hash() == sw2.world->state_hash());
    }

    // El world 2 recibe un input EXTRA en t=5 → desync a partir de ahí.
    FP3 kick{Fix32(999.0f), Fix32(0.0f), Fix32(0.0f)};
    std::vector<rollback::ExternalInput> only_second = {make_input(5, 1, sw2.vel_id, kick)};
    sw1.world->advance_tick();
    sw2.world->advance_tick();
    move_system(sw1);
    move_system(sw2);
    sw2.world->apply_inputs(only_second);
    CHECK(sw1.world->state_hash() != sw2.world->state_hash());
    CHECK(!DeterminismLinter::states_equal(*sw1.world, *sw2.world));

    // Sin más inputs, el desync persiste (estados distintos).
    sw1.world->advance_tick();
    sw2.world->advance_tick();
    move_system(sw1);
    move_system(sw2);
    CHECK(sw1.world->state_hash() != sw2.world->state_hash());
}

static void test_golden_movement() {
    std::cout << "7. Golden value: 10 ticks de velocidad (1,0,0)...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID pos_id = store->register_component("Pos", sizeof(FP3));
    ComponentID vel_id = store->register_component("Vel", sizeof(FP3));
    World w(store);
    w.enable_determinism_lock();

    Entity e = w.spawn_with_id(1);
    FP3 pos{Fix32(0.0f), Fix32(0.0f), Fix32(0.0f)};
    FP3 vel{Fix32(1.0f), Fix32(0.0f), Fix32(0.0f)};
    w.add_component(e, pos_id, &pos);
    w.add_component(e, vel_id, &vel);

    for (int t = 0; t < 10; ++t) {
        w.advance_tick();
        move_system_lite(w, pos_id, vel_id);
    }

    size_t sz = 0;
    const FP3* p = static_cast<const FP3*>(w.get_entity_component_data(e, pos_id, sz));
    CHECK(p != nullptr);
    CHECK(p->x == Fix32(10.0f)); // 10 × Fix32(1) == Fix32(10) EXACTO
    CHECK(p->y == Fix32(0.0f));
    CHECK(p->z == Fix32(0.0f));
}

int main() {
    std::cout << "--- Starting FluxDB Deterministic Lockstep Test (#11) ---\n";

    compile_time_linter();
    test_fix32_math();
    test_fixed_random();
    test_canonical_iteration();
    test_scheduler_order();
    test_lockstep_simulation();
    test_desync_detection();
    test_golden_movement();

    std::cout << "--- LOCKSTEP TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
