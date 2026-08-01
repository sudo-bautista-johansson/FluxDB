#include "../core/headers/query_plans.h"
#include "../core/headers/parser.h"
#include "../core/headers/planner.h"
#include "../core/headers/executor.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

// ─────────────────────────────────────────
//  Roadmap #5: Compiled Query Plans (Query JIT)
//  Plan cache por firma + strides precomputados + invalidación selectiva
// ─────────────────────────────────────────

using namespace fluxdb::ecs;

struct Position { float x, y, z; };
struct Health   { int hp; };
struct Speed    { float v; };

int main() {
    std::cout << "--- Starting FluxDB Compiled Query Plans Test (#5) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(Position));
    auto hp_id  = store->register_component("Health", sizeof(Health));
    auto spd_id = store->register_component("Speed", sizeof(Speed));

    World world(store, std::make_shared<HistoryManager>(600));

    // Estado inicial: 4 entidades con Position, 2 de ellas con Health
    Position p = {1.f, 2.f, 3.f};
    Health h = {100};
    for (int i = 0; i < 4; ++i) {
        Entity e = world.spawn();
        p.x = static_cast<float>(i + 1);
        world.add_component(e, pos_id, &p);
        if (i % 2 == 0) {
            world.add_component(e, hp_id, &h);
        }
    }

    // 1. Compilación + match inicial
    QueryHandle h_pos = world.create_query({pos_id});
    QueryHandle h_both = world.create_query({pos_id, hp_id});

    const QueryPlan* plan_pos = world.get_query_plan(h_pos);
    const QueryPlan* plan_both = world.get_query_plan(h_both);
    assert(plan_pos && plan_both);
    assert(plan_pos->matched_archetype_count() == 2);  // {pos} y {pos,hp}
    assert(plan_both->matched_archetype_count() == 1); // {pos,hp}

    uint64_t v0_pos = plan_pos->plan_version();

    std::cout << "1. Plans compiled with correct archetype matches (OK)\n";

    // 2. Iteración con valores correctos via offsets precomputados
    size_t count_pos = 0;
    size_t count_both = 0;
    for_each_in_query(world, h_pos, [&](Entity e, size_t row, const QueryRow& qr) {
        const Position* pos = static_cast<const Position*>(qr.get(pos_id));
        assert(pos && pos->x > 0.f && pos->x <= 4.f);
        ++count_pos;
    });
    for_each_in_query(world, h_both, [&](Entity, size_t, const QueryRow& qr) {
        const Position* pos = static_cast<const Position*>(qr.get(pos_id));
        const Health* hp = static_cast<const Health*>(qr.get(hp_id));
        assert(pos && hp && hp->hp == 100);
        ++count_both;
    });
    assert(count_pos == 4);
    assert(count_both == 2);

    std::cout << "2. Iteration reads correct values through precomputed offsets (OK)\n";

    // 3. Dedupe: misma firma -> mismo handle
    QueryHandle h_pos2 = world.create_query({pos_id});
    assert(h_pos2 == h_pos);

    std::cout << "3. Same signature deduped to same plan (OK)\n";

    // 4. Invalidación selectiva: un arquetipo NUEVO ({pos, speed}) se crea
    //    DESPUÉS de compilar los planes. Solo h_pos debe actualizarse.
    Speed sp = {10.f};
    for (int i = 0; i < 3; ++i) {
        Entity e = world.spawn();
        p.x = 100.f + i;
        world.add_component(e, pos_id, &p);
        world.add_component(e, spd_id, &sp);
    }

    const QueryPlan* plan_pos_after = world.get_query_plan(h_pos);
    const QueryPlan* plan_both_after = world.get_query_plan(h_both);
    assert(plan_pos_after->matched_archetype_count() == 3);  // + {pos,spd}
    assert(plan_both_after->matched_archetype_count() == 1); // intacto
    assert(plan_pos_after->plan_version() > v0_pos);
    assert(plan_both_after->plan_version() == plan_both->plan_version());

    size_t count_after = 0;
    for_each_in_query(world, h_pos, [&](Entity, size_t, const QueryRow&) { ++count_after; });
    assert(count_after == 7);

    std::cout << "4. Selective invalidation: only matching plans updated on new archetype (OK)\n";

    // 5. Filtro temporal integrado en el plan (#4): changed_since
    world.advance_tick(); // tick 1
    Position moved = {50.f, 0.f, 0.f};
    // mover 2 entidades (id 0 y 1) — se sobreescriben sus Position
    {
        // localizamos las primeras dos entidades via la query
        size_t idx = 0;
        Entity targets[2];
        for_each_in_query(world, h_pos, [&](Entity e, size_t, const QueryRow&) {
            if (idx < 2) targets[idx] = e;
            ++idx;
        });
        world.add_component(targets[0], pos_id, &moved);
        world.add_component(targets[1], pos_id, &moved);
    }

    size_t changed = 0;
    for_each_changed_in_query(world, h_pos, pos_id, 0, [&](Entity, size_t, const QueryRow& qr) {
        const Position* pos = static_cast<const Position*>(qr.get(pos_id));
        assert(pos && pos->x == 50.f);
        ++changed;
    });
    assert(changed == 2);

    size_t changed_after_tick1 = 0;
    for_each_changed_in_query(world, h_pos, pos_id, 1, [&](Entity, size_t, const QueryRow&) { ++changed_after_tick1; });
    assert(changed_after_tick1 == 0);

    std::cout << "5. Temporal filter built into the plan (changed_since, OK)\n";

    // 6. Sanity: reads from a plan whose archetype has no matching entity are empty
    QueryHandle h_speed = world.create_query({spd_id});
    size_t count_speed = 0;
    for_each_in_query(world, h_speed, [&](Entity, size_t, const QueryRow&) { ++count_speed; });
    assert(count_speed == 3);

    std::cout << "6. Standalone component query (OK)\n";

    // 7. Invalidación por remoción: al quitar el arquetipo {pos,spd}
    //    (vacío tras despawn), solo los planes que lo matcheaban se actualizan.
    //    Arquetipo {pos,spd}: entidades spawn-idas en el paso 4 (ids 4..6).
    uint64_t v_before_remove = plan_pos_after->plan_version();
    uint64_t v_both_before_remove = plan_both_after->plan_version();

    // Vaciar el arquetipo {pos,spd}
    std::vector<Entity> to_kill;
    for_each_in_query(world, h_speed, [&](Entity e, size_t, const QueryRow&) { to_kill.push_back(e); });
    for (Entity e : to_kill) world.despawn(e);

    ArchetypeSignature pos_spd_sig;
    pos_spd_sig.set(pos_id);
    pos_spd_sig.set(spd_id);
    assert(world.remove_archetype(pos_spd_sig));

    const QueryPlan* plan_pos_removed = world.get_query_plan(h_pos);
    const QueryPlan* plan_both_removed = world.get_query_plan(h_both);
    assert(plan_pos_removed->matched_archetype_count() == 2);  // - {pos,spd}
    assert(plan_both_removed->matched_archetype_count() == 1); // intacto
    assert(plan_pos_removed->plan_version() > v_before_remove);
    assert(plan_both_removed->plan_version() == v_both_before_remove);

    size_t count_pos_after_remove = 0;
    for_each_in_query(world, h_pos, [&](Entity, size_t, const QueryRow&) { ++count_pos_after_remove; });
    assert(count_pos_after_remove == 4); // solo las entidades {pos} y {pos,hp}

    // Remover algo inexistente / con entidades vivas / el arquetipo vacío falla
    ArchetypeSignature bogus; bogus.set(hp_id); bogus.set(spd_id);
    assert(!world.remove_archetype(bogus));
    ArchetypeSignature empty_sig;
    assert(!world.remove_archetype(empty_sig));
    assert(!world.remove_archetype(pos_spd_sig)); // ya no existe

    std::cout << "7. Archetype removal invalidates only matching plans (OK)\n";

    // 8. Reutilización del caché desde la capa SQL (#5): Executor registra
    //    tablas → create_query (dedupe por firma) y SELECT usa el plan.
    fluxdb::query::Executor executor(&world);
    executor.register_table("players", {pos_id, hp_id});

    // misma tabla → mismo handle (caché de World)
    fluxdb::ecs::QueryHandle h1 = executor.query_handle("players");
    fluxdb::ecs::QueryHandle h2 = executor.query_handle("players");
    assert(h1 != fluxdb::query::Executor::QUERY_INVALID);
    assert(h1 == h2);
    assert(h1 == h_both); // misma firma {pos,hp} → mismo plan que h_both

    // Tabla no registrada → INVALID
    assert(executor.query_handle("nope") == fluxdb::query::Executor::QUERY_INVALID);

    // SELECT * FROM players via Parser/Planner → ejecuta sobre el plan cacheado
    fluxdb::query::Parser parser("SELECT * FROM players;");
    auto ast = parser.parse();
    (void)ast;
    fluxdb::query::Planner planner("SELECT * FROM players;");
    auto plan = planner.plan();
    executor.execute(plan);

    size_t sql_rows = 0;
    fluxdb::ecs::for_each_in_query(world, h1, [&](Entity, size_t, const QueryRow&) { ++sql_rows; });
    assert(sql_rows == 2); // {pos,hp}: entidades pares del inicio

    std::cout << "8. SQL layer reuses the compiled plan cache (OK)\n";

    std::cout << "--- COMPILED QUERY PLANS TEST PASSED (#5) ---\n";
    return 0;
}
