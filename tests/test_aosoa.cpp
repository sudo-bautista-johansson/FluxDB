// FluxDB — Feature #2: AoSoA Native Layout
// Lanes SIMD de entidades con columnas SoA; iteración vectorizable.
#include "../core/headers/aosoa.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::aoso;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB AoSoA Layout Test (#2) ---\n";

    // 2 campos: x, y (p.ej. position 2D).
    AoSoABuffer buf(2);
    CHECK(buf.lane_width() == kLaneWidth);
    CHECK(buf.empty());

    // 20 entidades → 3 lanes (8+8+4).
    for (int i = 0; i < 20; ++i) {
        float v[2] = {static_cast<float>(i), static_cast<float>(i * 2)};
        buf.push_back(v);
    }
    CHECK(buf.size() == 20);
    CHECK(buf.lane_count() == 3);
    CHECK(buf.lane_width() == 8);
    // Último lane: 4 entidades ocupadas.
    CHECK(buf.lane_count() == 3);

    // Lectura/escritura por fila global.
    CHECK(std::fabs(buf.get(0, 0) - 0.0f) < 1e-5f);
    CHECK(std::fabs(buf.get(5, 0) - 5.0f) < 1e-5f);
    CHECK(std::fabs(buf.get(19, 1) - 38.0f) < 1e-5f);
    buf.set(5, 0, 100.0f);
    CHECK(std::fabs(buf.get(5, 0) - 100.0f) < 1e-5f);

    // Columnas SoA dentro de un lane: contiguas.
    const LaneFloat* col = buf.lane_field(0, 0);
    CHECK(std::fabs((*col)[0] - 0.0f) < 1e-5f);
    CHECK(std::fabs((*col)[7] - 7.0f) < 1e-5f);

    // Iteración de lanes.
    size_t lanes_visited = 0;
    buf.for_each_lane([&](size_t, size_t n) { lanes_visited += (n > 0 ? 1 : 0); });
    CHECK(lanes_visited == buf.lane_count());

    // Suma vectorizada (a=a+b): posición 0 += velocidad 1.
    // x = i + 2i = 3i.
    AoSoABuffer vel(2);
    for (int i = 0; i < 20; ++i) {
        float v[2] = {static_cast<float>(i), static_cast<float>(i)}; // velocidad
        vel.push_back(v);
    }
    size_t best_lane = vectorized_add(vel, 0, 1, nullptr);
    CHECK(std::fabs(vel.get(0, 0) - 0.0f) < 1e-5f);
    CHECK(std::fabs(vel.get(10, 0) - 20.0f) < 1e-5f);
    CHECK(std::fabs(vel.get(19, 0) - 38.0f) < 1e-5f);
    CHECK(best_lane == 2); // la mayor suma está en el último lane (i=19 → 38)

    // Simetría de lanes: entidades por lane son 8/8/4.
    CHECK(buf.lane_width() == 8);

    std::cout << "--- AOSOA LAYOUT TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}