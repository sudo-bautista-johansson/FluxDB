// FluxDB — Feature #3: GPU-Resident Mirror Archetypes
// Buffer espejo con upload selectivo de páginas sucias (costo PCIe).
#include "../core/headers/gpu_mirror.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace fluxdb::gpu;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB GPU Mirror Archetypes Test (#3) ---\n";

    // Componente: transform de 3 floats (12 bytes).
    const size_t comp_size = 12;
    const size_t entity_capacity = 1024; // 4 páginas de 256 filas

    MirroredArchetype arch(comp_size, entity_capacity);
    CHECK(arch.capacity_rows() == 1024);
    CHECK(arch.page_rows() == kPageRows);
    CHECK(arch.dirty_count() == 0);

    // Escribir 300 entidades (páginas 0 y 1 tocadas).
    float transform[3];
    for (size_t i = 0; i < 300; ++i) {
        transform[0] = static_cast<float>(i);
        transform[1] = static_cast<float>(i) * 2;
        transform[2] = 0;
        arch.set_component(i, transform);
    }
    CHECK(arch.dirty_count() == 2); // página 0 y 1 sucias

    // Upload selectivo: solo 2 de 4 páginas.
    size_t transferred = arch.upload_dirty_pages();
    // 2 páginas × 256 filas × 12 bytes.
    CHECK(transferred == 2 * 256 * comp_size);
    CHECK(arch.dirty_count() == 0);
    CHECK(arch.total_uploaded_bytes() == transferred);

    // Los datos del GPU buffer están correctos.
    float out[3];
    CHECK(arch.read_gpu(299, out));
    CHECK(std::fabs(out[0] - 299.0f) < 1e-5f);
    CHECK(std::fabs(out[1] - 598.0f) < 1e-5f);

    // Modificar 1 fila más → solo 1 página dirty, upload pequeño.
    transform[0] = 42.0f;
    arch.set_component(500, transform); // página 1
    CHECK(arch.dirty_count() == 1);
    size_t small = arch.upload_dirty_pages();
    CHECK(small == 1 * 256 * comp_size);
    CHECK(arch.total_uploaded_bytes() == transferred + small);

    // El total transferido es MENOR que el buffer completo (upload selectivo).
    CHECK(arch.total_uploaded_bytes() < arch.capacity_rows() * comp_size * 2);

    // Sin cambios → upload 0 bytes.
    CHECK(arch.upload_dirty_pages() == 0);

    std::cout << "--- GPU MIRROR ARCHETYPES TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}