#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #14: Native Blackboard & Utility Scoring Storage
//  (AI stack)
// ─────────────────────────────────────────────────────────────
// Los blackboards de IA y las tablas de utility scoring viven como
// componentes ECS densos (no objetos heap-por-agente). La evaluación
// de curvas de utilidad es una query vectorizable batch; el cambio
// de blackboard se rastrea con el versionado #4 para "re-evaluate
// only if inputs changed".

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace fluxdb {
namespace ai {

// Clave tipada de blackboard (id estándar por nombre).
struct BlackboardKey {
    uint32_t id = 0;
    BlackboardKey() = default;
    explicit BlackboardKey(uint32_t i) : id(i) {}
    BlackboardKey(const char* name) {
        // FNV-1a 32-bit sobre el nombre; estable entre ejecuciones.
        uint32_t h = 2166136261u;
        for (const char* p = name; *p; ++p) {
            h ^= static_cast<unsigned char>(*p);
            h *= 16777619u;
        }
        id = h;
    }
    bool operator==(const BlackboardKey& o) const { return id == o.id; }
};

// Valor genérico de blackboard: hasta 8 bytes tipados.
struct BlackboardValue {
    enum class Type : uint8_t { NONE, FLOAT, INT, BOOL } type = Type::NONE;
    union {
        float f = 0.0f;
        int32_t i;
        bool b;
    };

    BlackboardValue() = default;
    static BlackboardValue make_float(float v) { BlackboardValue r; r.type = Type::FLOAT; r.f = v; return r; }
    static BlackboardValue make_int(int32_t v) { BlackboardValue r; r.type = Type::INT; r.i = v; return r; }
    static BlackboardValue make_bool(bool v) { BlackboardValue r; r.type = Type::BOOL; r.b = v; return r; }

    float as_float() const { return type == Type::INT ? static_cast<float>(i) : f; }
};

// Un blackboard compacto: array de (key, value) por agente.
// Se guarda como UN componente por agente (denso, cache-friendly).
struct Blackboard {
    static constexpr uint32_t kMaxEntries = 16;
    uint32_t count = 0;
    uint32_t keys[kMaxEntries];
    BlackboardValue values[kMaxEntries];

    void set(uint32_t key, const BlackboardValue& v) {
        for (uint32_t i = 0; i < count; ++i) {
            if (keys[i] == key) { values[i] = v; return; }
        }
        if (count < kMaxEntries) { keys[count] = key; values[count] = v; ++count; }
    }

    const BlackboardValue* get(uint32_t key) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }

    void clear() { count = 0; }
};

// Punto de curva de utilidad: respuesta en un valor de entrada.
struct UtilityPoint {
    float x = 0.0f;  // entrada (p.ej. distancia normalizada, amenaza)
    float y = 0.0f;  // utilidad (0..1)
};

// Curva de utilidad linear-interpolada (típica de utility AI).
struct UtilityCurve {
    std::vector<UtilityPoint> points;

    float evaluate(float x) const {
        if (points.empty()) return 0.0f;
        if (x <= points.front().x) return points.front().y;
        if (x >= points.back().x) return points.back().y;
        for (size_t i = 1; i < points.size(); ++i) {
            if (x <= points[i].x) {
                const auto& a = points[i - 1];
                const auto& b = points[i];
                float t = (x - a.x) / (b.x - a.x);
                return a.y + t * (b.y - a.y);
            }
        }
        return points.back().y;
    }
};

// Evaluador batch: puntúa a muchos agentes contra una curva en un loop
// denso (vectorizable), leyendo su blackboard.
class UtilityScorer {
public:
    // Añade una curva puntuable por una key de blackboard.
    void add_curve(uint32_t blackboard_key, UtilityCurve curve, float weight = 1.0f) {
        curves_.push_back({blackboard_key, std::move(curve), weight});
    }

    // Puntúa UN agente: suma de curvas evaluadas sobre su blackboard.
    float score(const Blackboard& bb) const {
        float total = 0.0f;
        for (const auto& c : curves_) {
            const BlackboardValue* v = bb.get(c.key);
            if (!v) continue;
            total += c.curve.evaluate(v->as_float()) * c.weight;
        }
        return total;
    }

    // Puntúa N agentes (batch): out[i] = utilidad del i-ésimo.
    void score_batch(const std::vector<Blackboard>& boards, std::vector<float>& out) const {
        out.resize(boards.size());
        for (size_t i = 0; i < boards.size(); ++i) {
            out[i] = score(boards[i]);
        }
    }

    size_t curve_count() const { return curves_.size(); }

private:
    struct ScoredCurve {
        uint32_t key;
        UtilityCurve curve;
        float weight;
    };
    std::vector<ScoredCurve> curves_;
};

} // namespace ai
} // namespace fluxdb
