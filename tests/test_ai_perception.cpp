// FluxDB — Feature #15: Spatial Perception Indices
// Conos de visión + radios de audición generan eventos en lugar de
// polling de raycasts. Dedup de eventos (SPOTTED una vez, LOST al salir).
#include "../core/headers/perception.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::ai;
using namespace fluxdb::spatial;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_vision_cone_spotted_and_lost() {
    std::cout << "1. Cono de visión: SPOTTED una vez, LOST al salir...\n";

    SpatialIndex index(Bounds{-1000,-1000,-1000, 1000,1000,1000});
    PerceptionIndex per;
    per.set_tick(10);

    Entity obs = 100;
    Entity target = 200;

    // Observador en origen mirando +X; target delante.
    VisionCone cone;
    cone.observer = obs;
    cone.ox = 0; cone.oy = 0; cone.oz = 0;
    cone.fx = 1; cone.fy = 0; cone.fz = 0;
    cone.range = 50;
    per.add_vision(cone);

    index.update_entity(obs, 0, 0, 0);
    index.update_entity(target, 20, 0, 0);  // delante, dentro del cono

    std::vector<PerceptionEvent> events;
    per.process_target_positions(index, events);
    CHECK(events.size() == 1);
    CHECK(events[0].kind == PerceptionKind::SPOTTED);
    CHECK(events[0].observer == obs);
    CHECK(events[0].target == target);
    CHECK(events[0].tick == 10);

    // Mismo tick: el target sigue en el cono → NO re-emite.
    per.process_target_positions(index, events);
    CHECK(events.size() == 1);

    // El target se mueve detrás del observador → LOST.
    index.update_entity(target, -20, 0, 0);
    per.process_target_positions(index, events);
    CHECK(events.size() == 2);
    CHECK(events[1].kind == PerceptionKind::LOST);
    CHECK(events[1].target == target);

    // Fuera del cono lateral (dentro del rango pero FOV angosto).
    index.update_entity(target, 10, 40, 0);
    per.process_target_positions(index, events);
    CHECK(events.size() == 2); // ya estaba "lost", no se observa de nuevo
}

static void test_vision_fov_filter() {
    std::cout << "2. FOV: un target al lado NO se ve...\n";

    SpatialIndex index(Bounds{-1000,-1000,-1000, 1000,1000,1000});
    PerceptionIndex per;
    per.set_tick(1);

    Entity obs = 1;
    VisionCone cone;
    cone.observer = obs;
    cone.fx = 1; cone.fy = 0; cone.fz = 0;
    cone.range = 50;
    per.add_vision(cone);

    index.update_entity(obs, 0, 0, 0);
    index.update_entity(2, 10, 45, 0);  // 45° a un lado

    std::vector<PerceptionEvent> events;
    per.process_target_positions(index, events);
    CHECK(events.size() == 0); // dot = cos(77°) ≈ 0.22 < 0.8
}

static void test_hearing_radius() {
    std::cout << "3. Radio de audición y loudness...\n";

    PerceptionIndex per;
    per.set_tick(5);

    HearingRadius h;
    h.observer = 7;
    h.ox = 0; h.oy = 0; h.oz = 0;
    h.radius = 30;
    per.add_hearing(h);

    std::vector<PerceptionEvent> events;

    // Sonido a 20 (dentro del radio).
    SoundEvent s1; s1.sx = 20; s1.sy = 0; s1.sz = 0; s1.loudness = 1.0f;
    per.process_sound(s1, events);
    CHECK(events.size() == 1);
    CHECK(events[0].kind == PerceptionKind::SOUND_HEARD);
    CHECK(events[0].observer == 7);

    // Sonido a 50 (fuera).
    SoundEvent s2; s2.sx = 50; s2.sy = 0; s2.sz = 0; s2.loudness = 1.0f;
    per.process_sound(s2, events);
    CHECK(events.size() == 1); // sin evento nuevo

    // Sonido a 50 pero fuerte (loudness 2 → radio efectivo 60).
    SoundEvent s3; s3.sx = 50; s3.sy = 0; s3.sz = 0; s3.loudness = 2.0f;
    per.process_sound(s3, events);
    CHECK(events.size() == 2);
}

static void test_multiple_observers_dedup() {
    std::cout << "4. Varios observadores, dedup por observador...\n";

    SpatialIndex index(Bounds{-1000,-1000,-1000, 1000,1000,1000});
    PerceptionIndex per;
    per.set_tick(3);

    Entity obsA = 10, obsB = 20;
    VisionCone cA; cA.observer = obsA; cA.ox=0; cA.oy=0; cA.oz=0; cA.fx=1; cA.fy=0; cA.fz=0; cA.range=50;
    VisionCone cB; cB.observer = obsB; cB.ox=100; cB.oy=0; cB.oz=0; cB.fx=-1; cB.fy=0; cB.fz=0; cB.range=50;
    per.add_vision(cA);
    per.add_vision(cB);
    CHECK(per.observer_count() == 2);

    index.update_entity(obsA, 0, 0, 0);
    index.update_entity(obsB, 100, 0, 0);
    index.update_entity(30, 50, 0, 0); // el target en medio → lo ven ambos

    std::vector<PerceptionEvent> events;
    per.process_target_positions(index, events);
    CHECK(events.size() == 2); // un SPOTTED por observador
}

int main() {
    std::cout << "--- Starting FluxDB Spatial Perception Test (#15) ---\n";
    test_vision_cone_spotted_and_lost();
    test_vision_fov_filter();
    test_hearing_radius();
    test_multiple_observers_dedup();
    std::cout << "--- SPATIAL PERCEPTION TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}