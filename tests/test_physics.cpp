// FluxDB — Feature #13: Time-Travel Collision Queries
// Ray/volume queries contra el estado histórico del mundo, sin mutar
// el World (a diferencia de rollback). Lag compensation y kill-cams.
#include "../core/headers/ecs.h"
#include "../core/headers/physics.h"
#include "../core/headers/history.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::ecs;
using namespace fluxdb::physics;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_ray_math() {
    std::cout << "1. Intersecciones de rayo (esfera y AABB)...\n";

    // Rayo +X desde el origen
    Ray ray;
    ray.ox = 0; ray.oy = 0; ray.oz = 0;
    ray.dx = 1; ray.dy = 0; ray.dz = 0;

    // Esfera en (10, 0, 0), radio 2 → impacto a distancia 8
    float d = ray.intersect_sphere(10, 0, 0, 4.0f); // r² = 4
    CHECK(d > 7.9f && d < 8.1f);

    // AABB [8,12]×[-2,2]×[-2,2] → impacto a distancia 8
    float da = ray.intersect_aabb(8, -2, -2, 12, 2, 2);
    CHECK(da > 7.9f && da < 8.1f);

    // Esfera detrás del origen → -1
    float d2 = ray.intersect_sphere(-10, 0, 0, 4.0f);
    CHECK(d2 < 0);

    // Esfera que el rayo pasa de largo → -1
    float d3 = ray.intersect_sphere(10, 10, 0, 4.0f); // centro a 10 de alto, radio 2
    CHECK(d3 < 0);

    // AABB que el rayo no toca → -1
    float da2 = ray.intersect_aabb(8, 10, -2, 12, 14, 2);
    CHECK(da2 < 0);

    // Rayo apuntando a la esfera por un ángulo pero PASANDO DE LARGO:
    // r2 en y=-5, esfera radio 3 en y=0 → la mínima distancia es 5 > 3 → no hay hit
    Ray r2;
    r2.ox = 0; r2.oy = -5; r2.oz = 0;
    r2.dx = 1; r2.dy = 0; r2.dz = 0;
    float d4 = r2.intersect_sphere(10, 0, 0, 9.0f); // esfera radio 3 en y=0
    CHECK(d4 < 0); // pasa de largo (distancia mínima 5 > radio 3)
}

static void test_raycast_live() {
    std::cout << "2. Raycast contra el estado actual...\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(100);
    World world(store, history);

    ComponentID pos_id = store->register_component("Position", sizeof(float) * 3);
    world.set_position_component_id(pos_id);

    // Dos entidades: una en (0,0,0) y otra en (10,0,0)
    Entity e0 = world.spawn();
    Entity e1 = world.spawn();
    float p0[3] = {0, 0, 0};
    float p1[3] = {10, 0, 0};
    world.add_component(e0, pos_id, p0);
    world.add_component(e1, pos_id, p1);

    Ray ray;
    ray.ox = -5; ray.oy = 0; ray.oz = 0;
    ray.dx = 1; ray.dy = 0; ray.dz = 0;

    RaycastHit hit;
    CHECK(world.raycast(ray, 1.0f, 100.0f, hit)); // radio 1 (r²=1)
    CHECK(hit.hit);
    CHECK(hit.entity == e0); // la más cercana
    CHECK(hit.distance > 3.9f && hit.distance < 4.1f); // entrada a la esfera: 5 - 1

    // Con radio 1, la e1 (a 15 del origen del rayo) NO se alcanza si e0 está antes
    // Ambos hits: verificar que devuelve la más cercana (e0 a distancia 5)
    // Radio 0.5 → ninguna porque el rayo pasa a más de 0.5 de (0,0,0)? No: está en el centro.
    RaycastHit hit2;
    CHECK(world.raycast(ray, 0.1f, 100.0f, hit2)); // radio pequeño aún golpea el centro exacto
    CHECK(hit2.entity == e0);
}

static void test_raycast_historical() {
    std::cout << "3. Raycast histórico (posiciones del pasado)...\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(100);
    World world(store, history);

    ComponentID pos_id = store->register_component("Position", sizeof(float) * 3);
    world.set_position_component_id(pos_id);

    Entity e = world.spawn();
    float p0[3] = {0, 0, 0};
    world.add_component(e, pos_id, p0);

    // La entidad se mueve a (20,0,0) en tick 1
    world.advance_tick();
    float p1[3] = {20, 0, 0};
    world.add_component(e, pos_id, p1);

    world.advance_tick();
    float p2[3] = {40, 0, 0};
    world.add_component(e, pos_id, p2);

    // Rayo desde (-5,0,0) hacia +X:
    Ray ray;
    ray.ox = -5; ray.oy = 0; ray.oz = 0;
    ray.dx = 1; ray.dy = 0; ray.dz = 0;

    // En tick 0, la entidad estaba en (0,0,0) → entrada a distancia 4
    RaycastHit h0;
    CHECK(world.raycast_historical(0, ray, 1.0f, 100.0f, h0));
    CHECK(h0.entity == e);
    CHECK(h0.distance > 3.9f && h0.distance < 4.1f);

    // En tick 2, la entidad está en (40,0,0) → entrada a distancia 44
    RaycastHit h2;
    CHECK(world.raycast_historical(2, ray, 1.0f, 100.0f, h2));
    CHECK(h2.entity == e);
    CHECK(h2.distance > 43.9f && h2.distance < 44.1f);

    // Live (tick = UINT64_MAX): posición actual (40,0,0)
    RaycastHit hLive;
    CHECK(world.raycast(ray, 1.0f, 100.0f, hLive));
    CHECK(hLive.entity == e);
    CHECK(hLive.distance > 43.9f && hLive.distance < 44.1f);
}

static void test_query_volume_historical() {
    std::cout << "4. Query de volumen histórico...\n";

    auto store = std::make_shared<ComponentStore>();
    auto history = std::make_shared<HistoryManager>(100);
    World world(store, history);

    ComponentID pos_id = store->register_component("Position", sizeof(float) * 3);
    world.set_position_component_id(pos_id);

    Entity e0 = world.spawn();
    Entity e1 = world.spawn();
    float pa[3] = {0, 0, 0};
    float pb[3] = {100, 0, 0};
    world.add_component(e0, pos_id, pa);
    world.add_component(e1, pos_id, pb);

    // Moverse en tick 1
    world.advance_tick();
    float pa1[3] = {50, 0, 0};
    world.add_component(e0, pos_id, pa1);
    float pb1[3] = {20, 0, 0};
    world.add_component(e1, pos_id, pb1);

    // Volumen en tick 1: esfera en (0,0,0) radio 60 → NO incluye a e0 (50) ni a e1 (20)
    VolumeQuery vol;
    vol.shape = VolumeShape2::SPHERE;
    vol.cx = 0; vol.cy = 0; vol.cz = 0;
    vol.r = 60.0f;
    std::vector<uint32_t> hits;
    world.query_volume_historical(1, vol, hits);
    CHECK(hits.size() == 2); // e0 a 50 y e1 a 20 ambos dentro

    // AABB en tick 1: [0,30]×[-5,5]×[-5,5] → solo e1 (a 20)
    VolumeQuery vol2;
    vol2.shape = VolumeShape2::AABB;
    vol2.cx = 0; vol2.cy = -5; vol2.cz = -5;
    vol2.max_x = 30; vol2.max_y = 5; vol2.max_z = 5;
    hits.clear();
    world.query_volume_historical(1, vol2, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0] == e1);

    // En tick 0, e0 estaba en (0,0,0): esfera pequeña la incluye
    VolumeQuery vol3;
    vol3.shape = VolumeShape2::SPHERE;
    vol3.r = 5.0f;
    hits.clear();
    world.query_volume_historical(0, vol3, hits);
    CHECK(hits.size() == 1);
    CHECK(hits[0] == e0);
}

int main() {
    std::cout << "--- Starting FluxDB Time-Travel Collision Queries Test (#13) ---\n";
    test_ray_math();
    test_raycast_live();
    test_raycast_historical();
    test_query_volume_historical();
    std::cout << "--- TIME-TRAVEL COLLISION TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}