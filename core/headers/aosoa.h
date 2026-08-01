#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #2: AoSoA (Array-of-Structs-of-Arrays) Native Layout
//  (Phase 5 - Frontier / GPU)
// ─────────────────────────────────────────────────────────────
// Agrupa entidades en LANES de ancho SIMD (8). Dentro de cada lane,
// cada campo vive en un array contiguo (SoA) listo para registros
// AVX/NEON; los lanes se iteran en batch. Esto da vectorización +
// localidad multi-campo (position+velocity en el mismo lane).

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>
#include <cassert>

namespace fluxdb {
namespace aoso {

// Ancho de lane SIMD (filas por lane).
static constexpr size_t kLaneWidth = 8;

// Un campo numérico de un lane: kLaneWidth floats contiguos.
using LaneFloat = std::array<float, kLaneWidth>;

// Un lane de entidades: una columna SoA por campo.
// `num_fields` fijo en compile-time vía template en el uso típico;
// esta versión usa vector<LaneFloat> para flexibilidad.
struct Lane {
    std::vector<LaneFloat> fields;
    size_t num_entities = 0; // cuántas filas del lane están ocupadas

    Lane() = default;
    explicit Lane(size_t n_fields) : fields(n_fields) {}
};

// Buffer AoSoA: vector de lanes, cada uno con n_fields columnas.
class AoSoABuffer {
public:
    AoSoABuffer(size_t num_fields, size_t lane_width = kLaneWidth)
        : num_fields_(num_fields), lane_width_(lane_width) {}

    // Añade una entidad: escribe sus `num_fields` floats en el lane actual.
    void push_back(const float* values) {
        if (lanes_.empty() || lanes_.back().num_entities == lane_width_) {
            lanes_.emplace_back(num_fields_);
        }
        Lane& lane = lanes_.back();
        for (size_t f = 0; f < num_fields_; ++f) {
            lane.fields[f][lane.num_entities] = values[f];
        }
        ++lane.num_entities;
        ++total_;
    }

    // Lee el campo `field` de la fila global `index`.
    float get(size_t index, size_t field) const {
        size_t lane = index / lane_width_;
        size_t row = index % lane_width_;
        return lanes_[lane].fields[field][row];
    }

    // Escribe el campo `field` de la fila global `index`.
    void set(size_t index, size_t field, float v) {
        size_t lane = index / lane_width_;
        size_t row = index % lane_width_;
        lanes_[lane].fields[field][row] = v;
    }

    // Iterador de lane: expone la columna SoA (puntero a 8 floats).
    // El caller SIMD puede cargarlo directo a un registro.
    const LaneFloat* lane_field(size_t lane_idx, size_t field) const {
        return &lanes_[lane_idx].fields[field];
    }

    float* lane_field_mutable(size_t lane_idx, size_t field) {
        return lanes_[lane_idx].fields[field].data();
    }

    size_t lane_count() const { return lanes_.size(); }
    size_t lane_width() const { return lane_width_; }
    size_t num_fields() const { return num_fields_; }
    size_t size() const { return total_; }
    bool empty() const { return lanes_.empty(); }

    // Procesa una operación por lane: f(lane_idx, puntero a cada columna).
    // Permite al compilador auto-vectorizar sobre las columnas SoA.
    template <typename F>
    void for_each_lane(F&& f) const {
        for (size_t l = 0; l < lanes_.size(); ++l) {
            f(l, lanes_[l].num_entities);
        }
    }

private:
    size_t num_fields_;
    size_t lane_width_;
    std::vector<Lane> lanes_;
    size_t total_ = 0;
};

// Utilidad: suma vectorizada de 2 campos por lane (p.ej. position + velocity).
// Devuelve el lane donde ocurrió la mayor magnitud (para tests).
inline size_t vectorized_add(AoSoABuffer& buf, size_t a_field, size_t b_field,
                             float* max_magnitude_out) {
    float best = -1.0f;
    size_t best_lane = 0;
    for (size_t l = 0; l < buf.lane_count(); ++l) {
        float* a = buf.lane_field_mutable(l, a_field);
        float* b = buf.lane_field_mutable(l, b_field);
        for (size_t i = 0; i < buf.lane_width(); ++i) {
            float sum = a[i] + b[i];
            a[i] = sum;
            if (sum > best) { best = sum; best_lane = l; }
        }
    }
    if (max_magnitude_out) *max_magnitude_out = best;
    return best_lane;
}

} // namespace aoso
} // namespace fluxdb
