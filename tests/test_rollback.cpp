#include "../core/headers/ecs.h"
#include "../core/headers/rollback.h"
#include <iostream>
#include <cassert>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::rollback;

struct Health { int hp; };
struct Position { float x, y, z; };

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static int read_hp(World& world, Entity e, ComponentID hp_id) {
    size_t sz = 0;
    const Health* h = static_cast<const Health*>(world.get_entity_component_data(e, hp_id, sz));
    return h ? h->hp : -1;
}

static void test_snapshot_restore() {
    std::cout << "1. WorldSnapshot capture/restore...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));

    Entity e = world.spawn();
    Health h{100};
    world.add_component(e, hp, &h);
    world.advance_tick();
    h.hp = 80;
    world.add_component(e, hp, &h);

    WorldSnapshot snap;
    CHECK(!snap.captured());
    snap.capture(world);
    CHECK(snap.captured());
    CHECK(snap.tick() == 1);

    world.advance_tick();
    h.hp = 50;
    world.add_component(e, hp, &h);
    CHECK(read_hp(world, e, hp) == 50);

    snap.restore(world);
    CHECK(read_hp(world, e, hp) == 80);
    CHECK(world.current_tick() == 1);
    CHECK(world.entity_last_write_tick(e, hp) == 1);
}

static void test_rollback_and_resimulate() {
    std::cout << "2. rollback_to / resimulate...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));
    ComponentID pos = store->register_component("Position", sizeof(Position));

    SnapshotRingBuffer ring(16);
    world.attach_rollback(&ring);

    // Sin ring capturado: rollback falla
    CHECK(!world.rollback_to(0));

    Entity e1 = world.spawn();
    Entity e2 = world.spawn();
    Health h{100};
    world.add_component(e1, hp, &h);
    world.add_component(e2, hp, &h);
    Position p{1.0f, 2.0f, 3.0f};
    world.add_component(e1, pos, &p);

    ring.capture(world); // base en tick 0

    world.advance_tick(); // tick 1
    h.hp = 90;
    world.add_component(e1, hp, &h);
    ring.capture(world);

    world.advance_tick(); // tick 2
    h.hp = 80;
    world.add_component(e1, hp, &h);
    p.x = 10.0f;
    world.add_component(e1, pos, &p);
    ring.capture(world);

    world.advance_tick(); // tick 3
    h.hp = 70;
    world.add_component(e2, hp, &h);
    Entity e3 = world.spawn();
    h.hp = 60;
    world.add_component(e3, hp, &h);
    ring.capture(world);

    CHECK(ring.has_base());
    CHECK(ring.base_tick() == 0);
    CHECK(ring.latest_tick() == 3);
    CHECK(ring.covers_tick(2) && !ring.covers_tick(5));

    // Rollback a tick 2: e1 hp=80, pos.x=10, e2 hp=100, e3 no existe
    CHECK(world.rollback_to(2));
    CHECK(world.current_tick() == 2);
    CHECK(read_hp(world, e1, hp) == 80);
    CHECK(read_hp(world, e2, hp) == 100);
    size_t sz = 0;
    const Position* p2 = static_cast<const Position*>(world.get_entity_component_data(e1, pos, sz));
    CHECK(p2 != nullptr && p2->x == 10.0f);
    CHECK(world.get_entity_component_data(e3, hp, sz) == nullptr);
    // Los stamps se re-aplican en el tick original
    CHECK(world.entity_last_write_tick(e1, hp) == 2);

    // Rollback a la base
    CHECK(world.rollback_to(0));
    CHECK(read_hp(world, e1, hp) == 100);
    CHECK(read_hp(world, e2, hp) == 100);

    // Resimular 0 -> 3 restaura el estado final grabado
    CHECK(world.resimulate(0, 3));
    CHECK(world.current_tick() == 3);
    CHECK(read_hp(world, e1, hp) == 80);
    CHECK(read_hp(world, e2, hp) == 70);
    CHECK(read_hp(world, e3, hp) == 60);

    // Resimular 2 -> 3 (sin pasar por la base)
    CHECK(world.resimulate(2, 3));
    CHECK(world.current_tick() == 3);
    CHECK(read_hp(world, e1, hp) == 80);
    CHECK(read_hp(world, e2, hp) == 70);

    // Fuera de rango / futuro
    CHECK(!world.rollback_to(4));
    CHECK(!world.rollback_to(99));
}

static void test_eviction() {
    std::cout << "3. Ring eviction (capacidad)...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));

    SnapshotRingBuffer ring(2); // solo 2 deltas en memoria
    world.attach_rollback(&ring);

    Entity e = world.spawn();
    Health h{100};
    world.add_component(e, hp, &h);
    ring.capture(world); // base tick 0

    for (int tick = 1; tick <= 4; ++tick) {
        world.advance_tick();
        h.hp = 100 - tick * 10;
        world.add_component(e, hp, &h);
        ring.capture(world);
    }

    CHECK(ring.size() <= 2);
    CHECK(ring.latest_tick() == 4);
    // El tick 1 ya no está cubierto (delta evictado)
    CHECK(!ring.covers_tick(1));
    CHECK(ring.covers_tick(3) && ring.covers_tick(4));
    CHECK(!world.rollback_to(1));

    CHECK(world.rollback_to(3));
    CHECK(read_hp(world, e, hp) == 70);
    CHECK(world.current_tick() == 3);
}

static void test_queries_after_rollback() {
    std::cout << "4. Queries compiladas sobreviven al rollback...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));

    SnapshotRingBuffer ring(8);
    world.attach_rollback(&ring);

    Entity e = world.spawn();
    Health h{100};
    world.add_component(e, hp, &h);
    ring.capture(world);

    QueryHandle q = world.create_query({hp});
    CHECK(world.get_query_plan(q) != nullptr);

    world.advance_tick();
    h.hp = 90;
    world.add_component(e, hp, &h);
    ring.capture(world);

    CHECK(world.rollback_to(0));
    // Los planes se invalidaron en clear_all: recompilar bajo demanda
    q = world.create_query({hp});
    const QueryPlan* plan = world.get_query_plan(q);
    CHECK(plan != nullptr);
    CHECK(read_hp(world, e, hp) == 100);
}

static void test_cow_chunk_pages() {
    std::cout << "5. COW chunk pages (compartir, clonar, repuntar)...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));

    SnapshotRingBuffer ring(16);
    world.attach_rollback(&ring);

    // 300 entidades con Health: llenan 2 chunks (256 + 44)
    const int N = 300;
    std::vector<Entity> es(N);
    for (int i = 0; i < N; ++i) {
        es[i] = world.spawn();
        Health h{100};
        world.add_component(es[i], hp, &h);
    }
    ring.capture(world); // base COW en tick 0

    ArchetypeSignature hp_sig;
    hp_sig.set(hp);
    unsigned long long hp_hash = hp_sig.to_ullong();

    auto fetch_arch = [&]() -> const Archetype* {
        auto it = world.get_archetypes().find(hp_hash);
        return it == world.get_archetypes().end() ? nullptr : it->second.get();
    };

    // Tras el capture, las páginas están COMPARTIDAS (world + snapshot)
    const Archetype* arch = fetch_arch();
    CHECK(arch != nullptr);
    CHECK(arch->get_entity_count() == N);
    CHECK(arch->page_share_count(hp, 0) == 2);
    CHECK(arch->page_share_count(hp, 1) == 2);

    // Un write clona la página del chunk afectado (copy-on-write)
    world.advance_tick();
    Health h{50};
    world.add_component(es[0], hp, &h); // row 0 → chunk 0
    CHECK(fetch_arch()->page_share_count(hp, 0) == 1); // clonada
    CHECK(fetch_arch()->page_share_count(hp, 1) == 2); // intocada: sigue compartida

    // Cambio estructural: spawn de otra entidad con Health → chunk 1 (row 300)
    Entity e_new = world.spawn();
    Health h2{42};
    world.add_component(e_new, hp, &h2);
    CHECK(fetch_arch()->page_share_count(hp, 1) == 1); // clonada por el append

    // Rollback: repunta a las páginas históricas sin copiar payloads
    CHECK(world.rollback_to(0));
    arch = fetch_arch();
    CHECK(arch != nullptr);
    CHECK(arch->get_entity_count() == N);          // e_new desaparece
    CHECK(arch->page_share_count(hp, 0) == 2);     // repuntada a la compartida
    CHECK(arch->page_share_count(hp, 1) == 2);
    CHECK(read_hp(world, es[0], hp) == 100);       // datos históricos intactos
    CHECK(read_hp(world, es[N - 1], hp) == 100);
    size_t sz = 0;
    CHECK(world.get_entity_component_data(e_new, hp, sz) == nullptr);
    CHECK(world.current_tick() == 0);

    // Tras el rollback, los writes vuelven a clonar (COW funcional otra vez)
    world.advance_tick();
    Health h3{90};
    world.add_component(es[5], hp, &h3);
    CHECK(fetch_arch()->page_share_count(hp, 0) == 1);
    CHECK(world.rollback_to(0));
    CHECK(read_hp(world, es[5], hp) == 100);
}

static void test_resimulate_with_inputs() {
    std::cout << "6. resimulate con inputs externos (corrección gana)...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));

    SnapshotRingBuffer ring(16);
    world.attach_rollback(&ring);

    Entity e = world.spawn();
    Entity e2 = world.spawn();
    Health h{100};
    world.add_component(e, hp, &h);
    world.add_component(e2, hp, &h);
    ring.capture(world); // base tick 0

    const RelationKind KIND = 7;
    for (int tick = 1; tick <= 3; ++tick) {
        world.advance_tick();
        h.hp = 100 - tick * 10;
        world.add_component(e, hp, &h);
        ring.capture(world);
    }
    // tick 3: e hp=70, e2 hp=100 (nunca escrito después de la base)

    auto make_set = [&](uint64_t tick, int value) {
        rollback::ExternalInput in;
        in.tick = tick;
        in.op = rollback::InputOp::SET_COMPONENT;
        in.entity = e;
        in.comp_id = hp;
        Health v{value};
        in.data.assign(reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(Health));
        return in;
    };

    // (a) Convergencia: los mismos inputs → el mismo estado final grabado
    std::vector<rollback::ExternalInput> same = {make_set(1, 90), make_set(2, 80), make_set(3, 70)};
    CHECK(world.resimulate(0, 3, same));
    CHECK(world.current_tick() == 3);
    CHECK(read_hp(world, e, hp) == 70);

    // (b) Corrección gana: el input del tick 3 pisa el delta grabado
    std::vector<rollback::ExternalInput> corr = {make_set(3, 75)};
    CHECK(world.resimulate(0, 3, corr));
    CHECK(read_hp(world, e, hp) == 75);
    CHECK(world.current_tick() == 3);

    // (c) Input de relación en tick intermedio: sobrevive hasta el final
    rollback::ExternalInput rel;
    rel.tick = 2;
    rel.op = rollback::InputOp::ADD_RELATION;
    rel.entity = e;
    rel.kind = KIND;
    rel.dst = e2;
    rel.payload.set_as<uint32_t>(42);
    std::vector<rollback::ExternalInput> rels = {rel};
    CHECK(world.resimulate(0, 3, rels));
    CHECK(world.has_relation(e, KIND, e2));
    RelationPayload got;
    CHECK(world.get_relation(e, KIND, e2, got));
    CHECK(got.as<uint32_t>() == 42);
    CHECK(read_hp(world, e, hp) == 70); // el delta del tick 3 sigue aplicado

    // (d) Inputs fuera de rango se ignoran (tick 0 y futuro)
    std::vector<rollback::ExternalInput> oob = {make_set(0, 1), make_set(9, 2)};
    CHECK(world.resimulate(0, 3, oob));
    CHECK(read_hp(world, e, hp) == 70);

    // (e) REMOVE_RELATION: quita la arista re-aplicada
    rollback::ExternalInput rem;
    rem.tick = 3;
    rem.op = rollback::InputOp::REMOVE_RELATION;
    rem.entity = e;
    rem.kind = KIND;
    rem.dst = e2;
    std::vector<rollback::ExternalInput> rems = {rel, rem};
    CHECK(world.resimulate(0, 3, rems));
    CHECK(!world.has_relation(e, KIND, e2));
}

int main() {
    std::cout << "--- Starting FluxDB Rollback Netcode Test (#8) ---\n";

    test_snapshot_restore();
    test_rollback_and_resimulate();
    test_eviction();
    test_queries_after_rollback();
    test_cow_chunk_pages();
    test_resimulate_with_inputs();

    std::cout << "--- ROLLBACK TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
