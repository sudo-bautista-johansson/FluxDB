#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #3: GPU-Resident Mirror Archetypes
//  (Phase 5 - Frontier / GPU)
// ─────────────────────────────────────────────────────────────
// Arquetipos marcados `GPUResident` tienen su array de componentes
// respaldado en un buffer visible por GPU (upload heap persistente).
// Los writes del CPU pasan por un dirty-page tracker (por chunk); el
// sync sube SOLO los chunks modificados, no el buffer completo. Aquí se
// simula el "device buffer" con memoria CPU y el bus PCIe con un
// contador de bytes transferidos, para validar la semántica de upload
// selectivo sin depender de Vulkan/D3D12.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>

namespace fluxdb {
namespace gpu {

// Tamaño de página de upload (filas por chunk, misma convención que ECS).
static constexpr size_t kPageRows = 256;

// Marca de residencia GPU para un componente.
enum class MirrorMode : uint8_t {
    NONE,          // solo CPU
    MIRRORED,      // CPU + GPU (espejo)
    GPU_AUTHORITATIVE, // autoritativo en GPU (el CPU lee async)
};

// Un buffer GPU simulado: array de bytes en "device memory".
class SimulatedDeviceBuffer {
public:
    explicit SimulatedDeviceBuffer(size_t num_bytes) : data_(num_bytes, 0) {}

    size_t size() const { return data_.size(); }

    const uint8_t* data() const { return data_.data(); }

    void write(size_t offset, const void* src, size_t n) {
        std::memcpy(data_.data() + offset, src, n);
    }

private:
    std::vector<uint8_t> data_;
};

// Tracker de páginas sucias: qué chunks cambiaron desde el último upload.
// Copy del dirty tracking de #4 pero a nivel de buffer GPU.
class DirtyPageTracker {
public:
    explicit DirtyPageTracker(size_t num_pages) : dirty_(num_pages, false) {}

    void mark(size_t page) {
        if (page < dirty_.size()) dirty_[page] = true;
    }

    void clear() {
        for (size_t i = 0; i < dirty_.size(); ++i) dirty_[i] = false;
    }

    // Enumera páginas sucias.
    void collect(std::vector<size_t>& out) const {
        for (size_t i = 0; i < dirty_.size(); ++i) {
            if (dirty_[i]) out.push_back(i);
        }
    }

    size_t dirty_count() const {
        size_t n = 0;
        for (bool d : dirty_) if (d) ++n;
        return n;
    }

private:
    std::vector<bool> dirty_;
};

// Arquetipo espejado: componente respaldado en un buffer GPU simulado.
class MirroredArchetype {
public:
    MirroredArchetype(size_t component_size, size_t entity_capacity,
                      size_t page_rows = kPageRows)
        : comp_size_(component_size),
          page_rows_(page_rows),
          pages_((entity_capacity + page_rows - 1) / page_rows),
          cpu_(component_size * entity_capacity),
          gpu_(component_size * entity_capacity),
          dirty_(pages_.size()),
          num_entities_(0) {}

    // Escribe el componente de la fila `row` (CPU). Marca la página dirty.
    void set_component(size_t row, const void* data) {
        if (row >= capacity_rows()) return;
        std::memcpy(cpu_.data() + row * comp_size_, data, comp_size_);
        dirty_.mark(row / page_rows_);
        ++num_entities_;
    }

    // Upload selectivo: sube solo las páginas sucias al buffer GPU.
    // Devuelve bytes transferidos (costo PCIe simulado).
    size_t upload_dirty_pages() {
        std::vector<size_t> dirty_pages;
        dirty_.collect(dirty_pages);
        size_t bytes = 0;
        for (size_t page : dirty_pages) {
            size_t begin = page * page_rows_ * comp_size_;
            size_t end = begin + page_rows_ * comp_size_;
            if (end > gpu_.size()) end = gpu_.size();
            size_t n = end - begin;
            gpu_.write(begin, cpu_.data() + begin, n);
            bytes += n;
        }
        dirty_.clear();
        total_uploaded_ += bytes;
        return bytes;
    }

    // Total transferido acumulado (para verificar que NO se sube todo).
    size_t total_uploaded_bytes() const { return total_uploaded_; }

    // Lee un componente directamente del buffer GPU (como un compute shader).
    bool read_gpu(size_t row, void* out) const {
        if (row >= capacity_rows()) return false;
        std::memcpy(out, gpu_.data() + row * comp_size_, comp_size_);
        return true;
    }

    size_t page_rows() const { return page_rows_; }
    size_t dirty_count() const { return dirty_.dirty_count(); }
    size_t capacity_rows() const { return cpu_.size() / comp_size_; }

private:
    size_t comp_size_;
    size_t page_rows_;
    std::vector<size_t> pages_;
    std::vector<uint8_t> cpu_;
    SimulatedDeviceBuffer gpu_;
    DirtyPageTracker dirty_;
    size_t num_entities_ = 0;
    size_t total_uploaded_ = 0;
};

} // namespace gpu
} // namespace fluxdb
