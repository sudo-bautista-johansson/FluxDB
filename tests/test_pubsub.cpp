#include "../core/headers/ecs.h"
#include "../core/headers/pubsub.h"
#include "../core/headers/parser.h"
#include <iostream>
#include <cassert>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::query;

struct Position {
    float x, y, z;
};

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void set_pos(World& world, Entity e, ComponentID pos_id, float x, float y, float z) {
    Position p{x, y, z};
    world.add_component(e, pos_id, &p);
}

int main() {
    std::cout << "--- Starting FluxDB Spatial Pub/Sub Test (#9) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();

    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(Position));
    world.set_position_component_id(pos_id);

    // ── 1. Batching por network tick ──
    std::cout << "1. Batching enter/leave por network tick...\n";
    Entity player1 = world.spawn();

    bool p1_entered = false;
    bool p1_exited = false;

    uint32_t sub_id = pubsub->subscribe_spatial(
        "", 10.0f, 0.0f, 0.0f, 5.0f,
        [&](uint32_t e, bool entered) {
            if (e == player1) {
                if (entered) p1_entered = true;
                else p1_exited = true;
            }
        });

    set_pos(world, player1, pos_id, 0.0f, 0.0f, 0.0f); // fuera de la zona
    CHECK(!p1_entered);
    CHECK(!p1_exited);

    set_pos(world, player1, pos_id, 8.0f, 0.0f, 0.0f); // entra a la zona
    CHECK(!p1_entered); // aún en el batch: el callback NO se disparó aún
    pubsub->flush_events();
    CHECK(p1_entered);
    CHECK(!p1_exited);

    p1_entered = false;
    set_pos(world, player1, pos_id, 9.0f, 0.0f, 0.0f); // sigue dentro
    pubsub->flush_events();
    CHECK(!p1_entered);
    CHECK(!p1_exited);

    set_pos(world, player1, pos_id, 20.0f, 0.0f, 0.0f); // sale de la zona
    CHECK(!p1_exited); // batch pendiente
    pubsub->flush_events();
    CHECK(!p1_entered);
    CHECK(p1_exited);

    // relevancia actual del suscriptor (para el delta engine #7)
    CHECK(pubsub->relevant_entities(sub_id).count(player1) == 0);

    // ── 2. Volúmenes de interés: AABB y frustum ──
    std::cout << "2. Interest volumes (AABB / frustum)...\n";
    Entity b = world.spawn();
    set_pos(world, b, pos_id, 5.0f, 0.0f, 5.0f);

    InterestVolume aabb;
    aabb.shape = VolumeShape::AABB;
    aabb.cx = 0; aabb.cy = 0; aabb.cz = 0;
    aabb.max_x = 10; aabb.max_y = 10; aabb.max_z = 10;

    uint32_t aabb_sub = pubsub->subscribe_volume("", aabb, nullptr);
    world.flush_interest_events(); // backfill del volumen recién suscrito
    CHECK(pubsub->relevant_entities(aabb_sub).count(b) == 1);

    // Sale del AABB
    set_pos(world, b, pos_id, 12.0f, 0.0f, 5.0f);
    pubsub->flush_events();
    CHECK(pubsub->relevant_entities(aabb_sub).count(b) == 0);

    // Frustum: cámara en (0,0,0) mirando -Z, FOV ~90°, far=100
    InterestVolume frustum;
    frustum.shape = VolumeShape::FRUSTUM;
    frustum.cx = 0; frustum.cy = 0; frustum.cz = 0;
    frustum.fwd_x = 0; frustum.fwd_y = 0; frustum.fwd_z = -1;
    frustum.hfov = 0.7854f; // ~90° total horizontal
    frustum.vfov = 0.7854f;
    frustum.far_p = 100.0f;

    Entity front = world.spawn();
    set_pos(world, front, pos_id, 0.0f, 0.0f, -20.0f); // frente a la cámara
    Entity behind = world.spawn();
    set_pos(world, behind, pos_id, 0.0f, 0.0f, 20.0f); // detrás de la cámara
    Entity sideways = world.spawn();
    set_pos(world, sideways, pos_id, 100.0f, 0.0f, -20.0f); // fuera del FOV

    uint32_t fr_sub = pubsub->subscribe_volume("", frustum, nullptr);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(fr_sub).count(front) == 1);
    CHECK(pubsub->relevant_entities(fr_sub).count(behind) == 0);
    CHECK(pubsub->relevant_entities(fr_sub).count(sideways) == 0);

    // ── 3. Volumen de interés móvil (AOI de cámara) ──
    std::cout << "3. Moving interest volume (AOI)...\n";
    InterestVolume aoivol;
    aoivol.shape = VolumeShape::SPHERE;
    aoivol.cx = 0; aoivol.cy = 0; aoivol.cz = 0;
    aoivol.r = 10.0f;

    Entity mob = world.spawn();
    set_pos(world, mob, pos_id, 50.0f, 0.0f, 0.0f); // lejos del AOI inicial

    uint32_t mov_sub = pubsub->subscribe_volume("", aoivol, nullptr);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(mov_sub).count(mob) == 0);

    // El AOI se mueve a (50,0,0): mob queda dentro
    aoivol.cx = 50.0f;
    pubsub->update_volume(mov_sub, aoivol);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(mov_sub).count(mob) == 1);

    // El AOI se aleja: mob sale
    aoivol.cx = -50.0f;
    pubsub->update_volume(mov_sub, aoivol);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(mov_sub).count(mob) == 0);

    // ── 4. ECS: InterestVolume sigue al owner ──
    std::cout << "4. ECS integration (InterestVolume sigue al owner)...\n";
    ComponentID iv_id = store->register_component("InterestVolume", sizeof(InterestVolume));
    world.set_interest_volume_component_id(iv_id);

    Entity watcher = world.spawn();
    InterestVolume iv;
    iv.shape = VolumeShape::SPHERE;
    iv.cx = 0; iv.cy = 0; iv.cz = 0;
    iv.r = 10.0f;
    world.add_component(watcher, iv_id, &iv); // registra su AOI (backfill pendiente)

    Entity other = world.spawn();
    set_pos(world, other, pos_id, 8.0f, 0.0f, 0.0f); // dentro del AOI del watcher

    world.flush_interest_events(); // backfill
    uint32_t watcher_sub = world.interest_subscription(watcher);
    CHECK(watcher_sub != 0);
    CHECK(pubsub->relevant_entities(watcher_sub).count(other) == 1);

    // El watcher se mueve a (100,0,0): su AOI lo sigue y "other" sale
    set_pos(world, watcher, pos_id, 100.0f, 0.0f, 0.0f);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(watcher_sub).count(other) == 0);

    // ── 5. ECS: Replicated → relevancia de red automática ──
    std::cout << "5. Replicated filter (relevancia automática)...\n";
    ComponentID repl_id = store->register_component("Replicated", sizeof(uint8_t));
    world.set_replicated_component_id(repl_id);

    Entity plain = world.spawn();
    set_pos(world, plain, pos_id, 3.0f, 0.0f, 0.0f); // NO replicable, dentro del AOI
    Entity replicable = world.spawn();
    set_pos(world, replicable, pos_id, 5.0f, 0.0f, 0.0f); // replicable, dentro
    uint8_t marker = 1;
    world.add_component(replicable, repl_id, &marker);

    InterestVolume all;
    all.shape = VolumeShape::SPHERE;
    all.cx = 0; all.cy = 0; all.cz = 0;
    all.r = 100.0f;
    uint32_t repl_sub = pubsub->subscribe_replicated_volume("", all, nullptr);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(repl_sub).count(replicable) == 1);
    CHECK(pubsub->relevant_entities(repl_sub).count(plain) == 0);
    CHECK(pubsub->relevant_entities(repl_sub).count(watcher) == 0); // sin Replicated

    // ── 6. Despawn limpia la suscripción del volume owner ──
    std::cout << "6. Despawn cleanup...\n";
    world.despawn(watcher);
    CHECK(world.interest_subscription(watcher) == 0);
    CHECK(pubsub->relevant_entities(watcher_sub).empty()); // sub removida

    // Parser: LISTEN SELECT sigue funcionando
    Parser parser("LISTEN SELECT * FROM players WHERE distance(pos, [10, 0, 0]) < 5;");
    auto ast = parser.parse();
    auto* select_stmt = dynamic_cast<SelectStatement*>(ast.get());
    CHECK(select_stmt != nullptr);
    CHECK(select_stmt->is_listen == true);

    std::cout << "--- PUBSUB TEST PASSED (" << checks << " checks) ---\n";
    std::cout << "Interest-managed spatial pub/sub is fully working.\n";
    return 0;
}
