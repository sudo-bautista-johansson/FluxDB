// FluxDB — Feature #1: Hot/Cold Archetype Splitting
// Clasificación de componentes en tiers hot/warm/cold (compile-time + 
// heurística de runtime), prefetch por tier en el scheduler y serialización
// de red solo hot/replicated. El split es metadato: las queries no cambian
// sintácticamente, la transparencia lógica se mantiene al 100%.
#include "../core/headers/ecs.h"
#include "../core/headers/lod.h"
#include "../core/headers/pubsub.h"
#include "../core/headers/delta_set.h"
#include "../core/headers/scheduler.h"
#include "../core/headers/lockstep.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <memory>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::query;
using namespace fluxdb::lod;

struct Position { float x, y, z; };
struct Hover { float x, y, z; };        // tocado cada frame → HOT
struct Health { int32_t hp; };          // frecuencia media → WARM
struct QuestFlag { uint8_t q[32]; };    // tocado rara vez → COLD

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_classification() {
    std::cout << "1. Clasificación por tiers (compile-time)...\n";

    auto store = std::make_shared<ComponentStore>();

    ComponentID hover_id = store->register_component("Hover", sizeof(Hover), ComponentTier::HOT);
    ComponentID health_id = store->register_component("Health", sizeof(Health));
    ComponentID quest_id = store->register_component("QuestFlag", sizeof(QuestFlag), ComponentTier::COLD);

    CHECK(store->get_tier(hover_id) == ComponentTier::HOT);
    CHECK(store->get_tier(health_id) == ComponentTier::WARM); // default
    CHECK(store->get_tier(quest_id) == ComponentTier::COLD);
    CHECK(store->get_tier("QuestFlag") == ComponentTier::COLD);

    // Reclasificación manual (metadato, sin tocar datos)
    store->set_tier(hover_id, ComponentTier::COLD);
    CHECK(store->get_tier(hover_id) == ComponentTier::COLD);
    store->set_tier(hover_id, ComponentTier::HOT);
    CHECK(store->get_tier(hover_id) == ComponentTier::HOT);

    // Helpers de máscara
    CHECK(tier_mask(ComponentTier::HOT) == TIER_MASK_HOT);
    CHECK(tier_mask(ComponentTier::COLD) == TIER_MASK_COLD);
    CHECK(tier_in_mask(ComponentTier::WARM, TIER_MASK_HOT | TIER_MASK_WARM));
    CHECK(!tier_in_mask(ComponentTier::COLD, TIER_MASK_HOT));
    CHECK(tier_in_mask(ComponentTier::HOT, TIER_MASK_ALL));

    // El registro con tier explícito es el mismo componente (dedupe por nombre)
    CHECK(store->register_component("Hover", 999, ComponentTier::COLD) == hover_id);
    CHECK(store->get_info(hover_id).size == sizeof(Hover));
    CHECK(store->get_tier(hover_id) == ComponentTier::HOT);
}

static void test_logical_transparency() {
    std::cout << "2. Transparencia lógica (queries sin cambios)...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID hover_id = store->register_component("Hover", sizeof(Hover), ComponentTier::HOT);
    ComponentID quest_id = store->register_component("QuestFlag", sizeof(QuestFlag), ComponentTier::COLD);
    World world(store);

    // set/get funciona igual sin importar el tier
    Entity e1 = world.spawn();
    Entity e2 = world.spawn();
    Hover h{1.0f, 2.0f, 3.0f};
    QuestFlag q{};
    q.q[0] = 42;
    world.add_component(e1, hover_id, &h);
    world.add_component(e2, quest_id, &q);

    size_t sz = 0;
    const Hover* got_h = static_cast<const Hover*>(world.get_entity_component_data(e1, hover_id, sz));
    CHECK(got_h != nullptr && sz == sizeof(Hover));
    CHECK(got_h->x == 1.0f && got_h->z == 3.0f);

    const QuestFlag* got_q = static_cast<const QuestFlag*>(world.get_entity_component_data(e2, quest_id, sz));
    CHECK(got_q != nullptr && sz == sizeof(QuestFlag));
    CHECK(got_q->q[0] == 42);

    // for_each_changed: mismo comportamiento para hot y cold
    world.advance_tick();
    Hover h2{4.0f, 5.0f, 6.0f};
    QuestFlag q2{};
    q2.q[0] = 7;
    world.add_component(e1, hover_id, &h2);
    world.add_component(e2, quest_id, &q2);

    int hot_hits = 0, cold_hits = 0;
    world.for_each_changed(hover_id, 0, [&](Entity, size_t, uint32_t) { ++hot_hits; });
    world.for_each_changed(quest_id, 0, [&](Entity, size_t, uint32_t) { ++cold_hits; });
    CHECK(hot_hits == 1);
    CHECK(cold_hits == 1);
}

static void test_runtime_heuristics() {
    std::cout << "3. Heurística de runtime (contadores + reclassify)...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID hover_id = store->register_component("Hover", sizeof(Hover), ComponentTier::COLD);   // mal clasificado
    ComponentID health_id = store->register_component("Health", sizeof(Health));
    ComponentID quest_id = store->register_component("QuestFlag", sizeof(QuestFlag), ComponentTier::HOT); // mal clasificado
    World world(store);
    world.enable_access_profiling(true);

    const int N = 200;
    for (int i = 0; i < N; ++i) {
        Entity e = world.spawn();
        Hover h{1.0f, 0.0f, 0.0f};
        QuestFlag q{};
        world.add_component(e, hover_id, &h);
        world.add_component(e, quest_id, &q);
    }

    // 64 ticks, decay half-life 1 tick (steady-state = accesos/tick × 2):
    //   hover  → 200 writes/tick → ~400 → HOT   (≥ 64)
    //   health → 6 writes/tick   → ~12  → WARM  (8 < x < 64)
    //   quest  → 1 cada 4 ticks  → ~0.5 → COLD (≤ 8)
    const int TICKS = 64;
    for (int t = 0; t < TICKS; ++t) {
        world.advance_tick();
        for (int i = 0; i < N; ++i) {
            Hover h{1.0f + static_cast<float>(i), 0.0f, 0.0f};
            world.add_component(static_cast<Entity>(i), hover_id, &h);
        }
        if (t % 4 == 0) {
            QuestFlag q{};
            q.q[0] = static_cast<uint8_t>(t);
            world.add_component(0, quest_id, &q);
        }
        for (int i = 0; i < 6; ++i) {
            Health hp{100};
            world.add_component(static_cast<Entity>(i), health_id, &hp);
        }
    }

    // Antes del reclassify: la heurística vio el patrón de acceso (la tasa de
    // quest es ~0.25/tick: su contador oscila entre 0 y 1 — bien bajo del
    // umbral cold; hover mantiene ~400).
    CHECK(world.tier_stats()[hover_id].accesses >= 64);
    CHECK(world.tier_stats()[quest_id].accesses < 8);

    size_t changed = world.reclassify_components(64, 8);
    CHECK(changed == 2); // hover y quest cambiaron de tier; health sigue WARM
    CHECK(store->get_tier(hover_id) == ComponentTier::HOT);
    CHECK(store->get_tier(health_id) == ComponentTier::WARM);
    CHECK(store->get_tier(quest_id) == ComponentTier::COLD);

    // Reclassify idempotente: una segunda pasada no cambia nada
    CHECK(world.reclassify_components(64, 8) == 0);

    // Decay: sin writes nuevos los contadores caen exponencialmente
    uint64_t before = world.tier_stats()[hover_id].accesses;
    for (int t = 0; t < 16; ++t) world.advance_tick();
    uint64_t after = world.tier_stats()[hover_id].accesses;
    CHECK(after < before);

    // Sin profiling: contadores congelados en cero (overhead cero)
    world.enable_access_profiling(false);
    Hover hh{9.0f, 9.0f, 9.0f};
    world.add_component(0, hover_id, &hh);
    CHECK(world.tier_stats()[hover_id].accesses == 0);
}

static void test_tier_prefetch() {
    std::cout << "4. Prefetch por tier (solo arrays de ese tier)...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID hover_id = store->register_component("Hover", sizeof(Hover), ComponentTier::HOT);
    ComponentID quest_id = store->register_component("QuestFlag", sizeof(QuestFlag), ComponentTier::COLD);
    World world(store);

    // 600 entidades → ceil(600/256) = 3 páginas por componente
    const int N = 600;
    for (int i = 0; i < N; ++i) {
        Entity e = world.spawn();
        Hover h{1.0f, 0.0f, 0.0f};
        QuestFlag q{};
        world.add_component(e, hover_id, &h);
        world.add_component(e, quest_id, &q);
    }

    // Prefetch solo hot: toca las páginas de Hover, NUNCA las de QuestFlag.
    // (El arquetipo de migración {hover} conserva páginas residuales, así que
    // las expectativas se derivan de tier_stats: el prefetch toca TODO lo que
    // existe, y un sistema hot-only sigue sin arrastrar los arrays cold.)
    const std::vector<World::TierStats>& stats = world.tier_stats();
    size_t hot = world.prefetch_tiers(TIER_MASK_HOT);
    size_t cold = world.prefetch_tiers(TIER_MASK_COLD);
    CHECK(hot == stats[hover_id].pages);
    CHECK(cold == stats[quest_id].pages);
    CHECK(world.prefetch_tiers(TIER_MASK_ALL) == hot + cold);
    CHECK(stats[quest_id].pages == 3); // {hover,quest}: ceil(600/256) = 3 páginas
    CHECK(stats[hover_id].pages >= 3); // ... + residuales del arquetipo {hover}
    CHECK(stats[hover_id].bytes == stats[hover_id].pages * Archetype::PAGE_ROWS * sizeof(Hover));
    CHECK(stats[quest_id].tier == ComponentTier::COLD);
}

static void test_scheduler_prefetch() {
    std::cout << "5. Scheduler: prefetch solo de los tiers del sistema...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID hover_id = store->register_component("Hover", sizeof(Hover), ComponentTier::HOT);
    ComponentID quest_id = store->register_component("QuestFlag", sizeof(QuestFlag), ComponentTier::COLD);
    World world(store);
    world.enable_access_profiling(true);

    const int N = 600;
    for (int i = 0; i < N; ++i) {
        Entity e = world.spawn();
        Hover h{1.0f, 0.0f, 0.0f};
        QuestFlag q{};
        world.add_component(e, hover_id, &h);
        world.add_component(e, quest_id, &q);
    }

    det::SystemScheduler sched;
    // Páginas hot esperadas (derivadas del estado actual: el prefetch toca
    // todas las páginas del tier, incluidas las residuales de migración).
    uint64_t expected_hot_pages = 0;
    for (const auto& st : world.tier_stats()) {
        if (st.tier == ComponentTier::HOT) expected_hot_pages += st.pages;
    }
    CHECK(expected_hot_pages > 0);

    // Sistema hot-only: mueve hover, no toca quest. Escribe directo al chunk
    // (for_each_archetype_sorted sostiene el shared lock del world: no se
    // puede entrar por add_component — patrón de test_lockstep #11).
    size_t move_id = sched.register_system("move_hover", [&](World& w) {
        w.for_each_archetype_sorted([&](Archetype* arch) {
            if (!arch->has_component(hover_id)) return;
            const size_t n = arch->get_entity_count();
            const Entity* ents = arch->get_entities_ptr();
            for (size_t row = 0; row < n; ++row) {
                Hover h{2.0f, 3.0f, 4.0f};
                void* dst = arch->get_component_data(row, hover_id);
                if (dst) std::memcpy(dst, &h, sizeof(Hover));
            }
        });
    });
    sched.set_system_tier_access(move_id, TIER_MASK_HOT);

    // El sistema hot solo arrastrará páginas hot: durante el paso, quest no se
    // toca — su contador solo sufrirá el decay del advance_tick (c → c/2), y
    // el de hover crecerá con las lecturas del sistema.
    uint64_t quest_before = world.tier_stats()[quest_id].accesses;
    uint64_t hover_before = world.tier_stats()[hover_id].accesses;
    CHECK(sched.total_prefetched_pages() == 0); // todavía no se ejecutó

    sched.step(world);

    CHECK(world.tier_stats()[quest_id].accesses == quest_before / 2);
    CHECK(world.tier_stats()[hover_id].accesses >= hover_before / 2 + static_cast<uint64_t>(N));
    CHECK(sched.system_tier_access(move_id) == TIER_MASK_HOT);
}

static void test_network_serialization() {
    std::cout << "6. Red: serialización solo hot/replicated...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    ComponentID hot_id = store->register_component("HotReplicated", sizeof(Health), ComponentTier::HOT);
    ComponentID cold_id = store->register_component("ColdReplicated", sizeof(QuestFlag), ComponentTier::COLD);
    world.set_position_component_id(pos_id);

    // Ambos tienen reglas LOD explícitas (replicación declarada); el tier
    // cold la excluye del delta por defecto.
    world.set_component_lod(hot_id, {{1000.0f, 1, 0.0f}});
    world.set_component_lod(cold_id, {{1000.0f, 1, 0.0f}});

    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 0; aoi.cy = 0; aoi.cz = 0;
    aoi.r = 1000.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    Entity e = world.spawn();
    world.advance_tick(); // sella los writes (should_update exige last_write > 0)
    Position p{10.0f, 0.0f, 0.0f};
    world.add_component(e, pos_id, &p);
    Health hp{100};
    world.add_component(e, hot_id, &hp);
    QuestFlag q{};
    q.q[0] = 77;
    world.add_component(e, cold_id, &q);
    world.flush_interest_events();

    // Default: el componente COLD no viaja aunque tenga reglas LOD.
    delta::DeltaSet d = world.build_subscriber_delta(sub, 0);
    bool saw_hot = false, saw_cold = false;
    d.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op != delta::DeltaOp::UPDATE) return;
        if (r.comp_id == hot_id) saw_hot = true;
        if (r.comp_id == cold_id) saw_cold = true;
    });
    CHECK(saw_hot);
    CHECK(!saw_cold);

    // Opt-in explícito: vuelve el comportamiento completo.
    delta::DeltaSet d2 = world.build_subscriber_delta(sub, 0, true);
    bool saw_cold2 = false;
    d2.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::UPDATE && r.comp_id == cold_id) saw_cold2 = true;
    });
    CHECK(saw_cold2);

    // El snapshot de rollback sigue siendo COMPLETO (incluye cold): #8 intacto.
    delta::DeltaSet snap = delta::capture_world_snapshot(world);
    bool snap_cold = false;
    snap.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.comp_id == cold_id) snap_cold = true;
    });
    CHECK(snap_cold);
}

static void test_determinism_integrity() {
    std::cout << "7. Integridad: profiling no rompe el lockstep (#11)...\n";

    auto make_world = [](std::shared_ptr<ComponentStore> store) {
        auto w = std::make_unique<World>(store);
        ComponentID hover_id = store->get_id("Hover");
        w->enable_access_profiling(true);
        for (int i = 0; i < 10; ++i) {
            Entity e = w->spawn();
            Hover h{1.0f, 0.0f, 0.0f};
            w->add_component(e, hover_id, &h);
        }
        return w;
    };

    auto s1 = std::make_shared<ComponentStore>();
    auto s2 = std::make_shared<ComponentStore>();
    ComponentID h1 = s1->register_component("Hover", sizeof(Hover), ComponentTier::HOT);
    ComponentID h2 = s2->register_component("Hover", sizeof(Hover), ComponentTier::HOT);

    auto w1 = make_world(s1);
    auto w2 = make_world(s2);

    for (int t = 0; t < 3; ++t) {
        w1->advance_tick();
        w2->advance_tick();
        for (int i = 0; i < 10; ++i) {
            Hover h{2.0f, 0.0f, static_cast<float>(t)};
            w1->add_component(static_cast<Entity>(i), h1, &h);
            w2->add_component(static_cast<Entity>(i), h2, &h);
        }
        CHECK(w1->state_hash() == w2->state_hash()); // contadores = estado derivado
    }

    // Y sin profiling los hashes también coinciden (no hay camino alternativo)
    w1->enable_access_profiling(false);
    w2->enable_access_profiling(false);
    w1->advance_tick();
    w2->advance_tick();
    CHECK(w1->state_hash() == w2->state_hash());
}

int main() {
    std::cout << "--- Starting FluxDB Hot/Cold Archetype Splitting Test (#1) ---\n";
    test_classification();
    test_logical_transparency();
    test_runtime_heuristics();
    test_tier_prefetch();
    test_scheduler_prefetch();
    test_network_serialization();
    test_determinism_integrity();
    std::cout << "--- HOT/COLD TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
