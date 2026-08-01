// FluxDB — Feature #22: GPU-Driven Procedural Generation Pipeline
// Generación determinista en "GPU simulado" + readback selectivo.
#include "../core/headers/procgen.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb;
using namespace fluxdb::gen;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB GPU Procedural Generation Test (#22) ---\n";

    ProcGenPipeline pipe(4096);

    // 1000 instancias en una región 0..100, readback SOLO pares (50%).
    det::SeedChain sc;
    sc.sector_x = 7; sc.sector_y = 3; sc.sector_z = 0;
    sc.local_index = 0; sc.world_seed = 0xDEADBEEF;

    ProcGenParams p;
    p.seed = sc;
    p.instance_count = 1000;
    p.min_x = 0; p.min_y = 0; p.min_z = 0;
    p.max_x = 100; p.max_y = 100; p.max_z = 100;
    p.readback_mask = 1; // bit 0 = "tiene colisión" → 50% de las instancias

    size_t n = pipe.generate(p);
    CHECK(n == 1000);

    // Readback selectivo: solo índices pares → 500 instancias.
    ReadbackRequest req = pipe.readback(p);
    CHECK(req.pending);
    CHECK(req.results.size() == 500 * 3);

    // Determinismo: misma seed ⇒ misma secuencia.
    CHECK(pipe.verify_identical(p));

    // Rango correcto: todos dentro de [0,100).
    bool in_range = true;
    for (size_t i = 0; i < req.results.size(); ++i) {
        if (req.results[i] < 0.0f || req.results[i] >= 100.0f) in_range = false;
    }
    CHECK(in_range);

    // Dos pipelines con la misma seed producen idénticos resultados.
    ProcGenPipeline pipe2(4096);
    pipe2.generate(p);
    ReadbackRequest req2 = pipe2.readback(p);
    CHECK(req.results.size() == req2.results.size());
    bool same = true;
    for (size_t i = 0; i < req.results.size(); ++i) {
        if (std::fabs(req.results[i] - req2.results[i]) > 1e-5f) same = false;
    }
    CHECK(same);

    // Cambiar la seed → resultados distintos.
    det::SeedChain sc2 = sc;
    sc2.world_seed = 0x12345678;
    ProcGenParams p2 = p;
    p2.seed = sc2;
    pipe2.generate(p2);
    ReadbackRequest req3 = pipe2.readback(p2);
    bool different = false;
    for (size_t i = 0; i < req3.results.size(); ++i) {
        if (std::fabs(req3.results[i] - req.results[i]) > 1e-5f) { different = true; break; }
    }
    CHECK(different);

    // instance_count 0 → no pending, sin resultados.
    ProcGenParams empty = p;
    empty.instance_count = 0;
    pipe.generate(empty);
    ReadbackRequest req0 = pipe.readback(empty);
    CHECK(!req0.pending);
    CHECK(req0.results.empty());

    // Todos los datos → máscara de todos los bits: readback casi completo
    // (i=0 da 0 & mask = 0, excluido).
    ProcGenParams all = p;
    all.readback_mask = 0xFFFFFFFF;
    ReadbackRequest reqAll = pipe.readback(all);
    CHECK(reqAll.results.size() == 999 * 3);

    std::cout << "--- GPU PROC GEN TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}