// FluxDB — Feature #20: Priority-Based Chunk Streaming with Predictive Prefetch
// Carga/descarga de chunks por prioridad = distancia + velocidad extrapolada
// + frustum de cámara; estados DORMANT/LOADING/ACTIVE con presupuesto.
#include "../core/headers/ecs.h"
#include "../core/headers/streaming.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::ecs;
using namespace fluxdb::streaming;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_chunk_states_and_registration() {
    std::cout << "1. Registro y estados de chunks...\n";

    StreamingManager sm;
    sm.ensure_chunk(0, 0, 0);
    sm.ensure_chunk(1, 0, 0);
    sm.ensure_chunk(0, 1, 0);
    CHECK(sm.total_count() == 3);
    CHECK(sm.active_count() == 0);

    const StreamChunk* c = sm.get_chunk(0, 0, 0);
    CHECK(c != nullptr);
    CHECK(c->state == ChunkState::DORMANT);

    sm.set_chunk_state(0, 0, 0, ChunkState::ACTIVE);
    CHECK(sm.get_chunk(0, 0, 0)->state == ChunkState::ACTIVE);
    CHECK(sm.active_count() == 1);
}

static void test_priority_scoring() {
    std::cout << "2. Prioridad por distancia y prefetch predictivo...\n";

    StreamingManager sm;
    sm.set_radii(1000.0f, 1600.0f);
    sm.set_max_load(8);

    // Observador en el origen, quieto (sin velocidad).
    StreamObserver obs;
    obs.x = 0; obs.y = 0; obs.z = 0;
    obs.vx = 0; obs.vy = 0; obs.vz = 0;
    obs.fx = 1; obs.fy = 0; obs.fz = 0; // mirando +X

    // Chunks alrededor del origen.
    sm.ensure_chunk(0, 0, 0);    // en el origen → carga
    sm.ensure_chunk(1, 0, 0);    // a 1024 → dentro del radio de carga
    sm.ensure_chunk(2, 0, 0);    // a 2048 → fuera del prefetch
    sm.ensure_chunk(0, 5, 0);    // lejos en Y
    sm.ensure_chunk(-1, 0, 0);   // a -1024 → en el radio pero detrás (frustum −X)

    sm.update(obs, 1);

    // Solo el chunk del origen (dist 0 ≤ load_radius 1000) se carga.
    CHECK(sm.to_load().size() == 1);
    CHECK(sm.to_load()[0]->cx == 0 && sm.to_load()[0]->cy == 0);

    // El chunk lejano (0,5,0) a 5120 → fuera del prefetch, no se carga.
    for (auto* c : sm.to_load()) {
        CHECK(!(c->cx == 0 && c->cy == 5));
    }
}

static void test_predictive_prefetch() {
    std::cout << "3. El prefetch predictivo por velocidad prioriza el destino...\n";

    StreamingManager sm;
    sm.set_radii(1000.0f, 1600.0f);
    sm.set_max_load(16);

    // Chunks dispuestos: uno en el destino extrapolado (a ~3 segundos de
    // movimiento a 500 u/s → 1500 u) y otros a igual distancia actual.
    sm.ensure_chunk(0, 0, 0);
    sm.ensure_chunk(2, 0, 0);  // a 2048 en X (destino de la velocidad +X)

    StreamObserver obs;
    obs.x = 0; obs.y = 0; obs.z = 0;
    obs.vx = 600; obs.vy = 0; obs.vz = 0; // moviéndose +X a 600 u/s (2s → 1200)
    obs.fx = 1; obs.fy = 0; obs.fz = 0;

    sm.update(obs, 1);

    // Con velocidad, el chunk (2,0,0) (a 2048) tiene prioridad de prefetch
    // alta → entra en to_load aunque esté más allá del radio de carga.
    bool prefetched_dest = false;
    for (auto* c : sm.to_load()) {
        if (c->cx == 2 && c->cy == 0) prefetched_dest = true;
    }
    CHECK(prefetched_dest);
}

static void test_active_unload() {
    std::cout << "4. Descarga de chunks activos fuera de rango...\n";

    StreamingManager sm;
    sm.set_radii(1000.0f, 1600.0f);

    sm.ensure_chunk(0, 0, 0);
    sm.set_chunk_state(0, 0, 0, ChunkState::ACTIVE);
    sm.ensure_chunk(10, 0, 0); // a 10240
    sm.set_chunk_state(10, 0, 0, ChunkState::ACTIVE);

    StreamObserver obs;
    obs.x = 0; obs.y = 0; obs.z = 0;
    obs.vx = 0; obs.vy = 0; obs.vz = 0;
    obs.fx = 1; obs.fy = 0; obs.fz = 0;

    sm.update(obs, 1);

    // El chunk lejano activo se marca para descarga.
    bool will_unload_far = false;
    for (auto* c : sm.to_unload()) if (c->cx == 10 && c->cy == 0) will_unload_far = true;
    CHECK(will_unload_far);

    // El del origen NO se descarga.
    for (auto* c : sm.to_unload()) {
        CHECK(!(c->cx == 0 && c->cy == 0));
    }
}

int main() {
    std::cout << "--- Starting FluxDB Priority-Based Chunk Streaming Test (#20) ---\n";
    test_chunk_states_and_registration();
    test_priority_scoring();
    test_predictive_prefetch();
    test_active_unload();
    std::cout << "--- STREAMING TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}