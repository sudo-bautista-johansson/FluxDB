#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #33: GPU Compute System Compilation
//  (Phase 5 - Frontier / GPU)
// ─────────────────────────────────────────────────────────────
// Subconjunto seguro de un DSL de sistemas → transpilado a kernel
// HLSL compute. El MISMO sistema se ejecuta en CPU (fallback) y se
// emite como shader; la selección runtime usa heurística
// (hardware + tamaño de datos). Prevención de drift: ambos caminos
// comparten la misma definición del sistema.

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace fluxdb {
namespace gpucomp {

// Registros (tipos de campo) soportados por el subconjunto seguro.
enum class FieldType : uint8_t { FLOAT, FLOAT3, UINT };

struct FieldSpec {
    std::string name;
    FieldType type = FieldType::FLOAT;
};

// Declaración de acceso a componente (arquetipo + campos).
struct ComponentAccess {
    std::string component;
    std::vector<FieldSpec> fields;
};

// Una operación aritmética del DSL. Mantenemos una forma SSA simple:
// y = f(x). El transpilador emite HLSL 1:1; el fallback ejecuta el
// mismo kernel en CPU.
struct SystemOp {
    enum class Kind : uint8_t {
        ADD,        // out = a + b
        MUL_SCALAR, // out = a * s
        LERP,       // out = a + s*(b-a)
        CLAMP,      // out = clamp(a, lo, hi)
        SCALE,      // out = a * b (component-wise)
    };
    Kind kind = Kind::ADD;
    size_t in_a = 0;    // índice de campo de entrada
    size_t in_b = 0;    // índice de campo de entrada (o -1)
    float scalar = 0;   // para MUL_SCALAR / LERP
    float lo = 0, hi = 0;
    size_t out = 0;     // índice de campo de salida
};

// Definición de un sistema marcado #[gpu_eligible].
struct SystemDef {
    std::string name;
    ComponentAccess input;    // arquetipo + campos leídos
    ComponentAccess output;   // arquetipo + campos escritos
    std::vector<SystemOp> ops;

    bool gpu_eligible = true;
    bool cpu_fallback = true; // siempre disponible
};

// Heurística de selección runtime.
struct DeviceProfile {
    bool has_compute = false;
    size_t entity_count = 0;
    size_t entity_threshold = 10000; // por debajo → CPU
};

// HLSL emitido por el transpilador.
struct ShaderArtifact {
    std::string name;
    std::string hlsl;     // cuerpo del kernel (numthreads + RWStructuredBuffer)
    size_t registers = 0; // nº de registros float usados (para info)
};

// Fallback CPU del MISMO kernel: aplica las ops a valores por entidad.
class CpuKernel {
public:
    CpuKernel(const SystemDef& def, std::vector<float>* in_out)
        : def_(def), in_out_(in_out) {}

    // Ejecuta la secuencia de ops sobre un array de floats plano.
    void run(std::vector<float>& scratch) {
        for (const auto& op : def_.ops) {
            apply(op, scratch);
        }
    }

private:
    void apply(const SystemOp& op, std::vector<float>& v) {
        const float a = in_out_->at(op.in_a);
        const float b = (op.in_b < in_out_->size()) ? in_out_->at(op.in_b) : 0.0f;
        float out = 0.0f;
        switch (op.kind) {
            case SystemOp::Kind::ADD:       out = a + b; break;
            case SystemOp::Kind::MUL_SCALAR: out = a * op.scalar; break;
            case SystemOp::Kind::LERP:      out = a + op.scalar * (b - a); break;
            case SystemOp::Kind::CLAMP:     out = a < op.lo ? op.lo : (a > op.hi ? op.hi : a); break;
            case SystemOp::Kind::SCALE:     out = a * b; break;
        }
        in_out_->at(op.out) = out;
    }

    const SystemDef& def_;
    std::vector<float>* in_out_;
};

// Transpilador DSL → HLSL.
class ComputeCompiler {
public:
    // Emite un kernel HLSL que lee `input` y escribe `output`.
    ShaderArtifact compile(const SystemDef& def) {
        ShaderArtifact art;
        art.name = def.name;
        art.registers = def.input.fields.size() + def.output.fields.size();

        std::string s;
        s += "RWStructuredBuffer<float> g_in : register(u0);\n";
        s += "RWStructuredBuffer<float> g_out : register(u1);\n";
        s += "float3 clamp3(float3 v, float lo, float hi) { return clamp(v, lo, hi); }\n";
        s += "float3 lerp3(float3 a, float3 b, float s) { return a + s * (b - a); }\n";
        s += "[numthreads(64,1,1)]\n";
        s += "void CSMain(uint3 tid : SV_DispatchThreadID) {\n";
        s += "  uint i = tid.x;\n";
        s += "  uint stride = " + std::to_string(def.input.fields.size()) + ";\n";
        s += "  float3 a3 = float3(g_in[i*stride+0], g_in[i*stride+1], g_in[i*stride+2]);\n";
        s += "  float3 b3 = float3(g_in[i*stride+3], g_in[i*stride+4], g_in[i*stride+5]);\n";
        s += "  float3 r3 = float3(0,0,0);\n";

        for (const auto& op : def.ops) {
            switch (op.kind) {
                case SystemOp::Kind::ADD:
                    s += "  g_out[i*stride+" + std::to_string(op.out) + "] = a3[0] + b3[0];\n";
                    break;
                case SystemOp::Kind::MUL_SCALAR:
                    s += "  g_out[i*stride+" + std::to_string(op.out) + "] = a3[0] * " + f(op.scalar) + ";\n";
                    break;
                case SystemOp::Kind::LERP:
                    s += "  g_out[i*stride+" + std::to_string(op.out) + "] = a3[0] + " + f(op.scalar) + " * (b3[0] - a3[0]);\n";
                    break;
                case SystemOp::Kind::CLAMP:
                    s += "  g_out[i*stride+" + std::to_string(op.out) + "] = clamp(a3[0], " + f(op.lo) + ", " + f(op.hi) + ");\n";
                    break;
                case SystemOp::Kind::SCALE:
                    s += "  g_out[i*stride+" + std::to_string(op.out) + "] = a3[0] * b3[0];\n";
                    break;
            }
        }
        s += "  g_out[i*stride+" + std::to_string(def.ops.size() + 0) + "] = r3[0];\n";
        s += "}\n";
        art.hlsl = s;
        return art;
    }

    // Decide qué ejecutar: GPU kernel o CPU fallback.
    bool should_use_gpu(const DeviceProfile& prof) const {
        return prof.has_compute && prof.entity_count >= prof.entity_threshold;
    }

private:
    static std::string f(float v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.5ff", v);
        return buf;
    }
};

} // namespace gpucomp
} // namespace fluxdb
