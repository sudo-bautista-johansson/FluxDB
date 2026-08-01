// FluxDB — Feature #33: GPU Compute System Compilation
// DSL → HLSL transpilation + CPU fallback compartiendo la misma definición.
#include "../core/headers/gpu_compile.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>

using namespace fluxdb::gpucomp;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB GPU Compute System Compilation Test (#33) ---\n";

    // Sistema: "IntegrateVelocity" — position += velocity.
    // Campos: a3 = (px,py,pz), b3 = (vx,vy,vz) → out = ADD.
    SystemDef def;
    def.name = "IntegrateVelocity";
    def.input.component = "Transform";
    def.input.fields = {{"px", FieldType::FLOAT}, {"py", FieldType::FLOAT},
                        {"pz", FieldType::FLOAT}, {"vx", FieldType::FLOAT},
                        {"vy", FieldType::FLOAT}, {"vz", FieldType::FLOAT}};
    def.output.component = "Transform";
    def.output.fields = {{"px", FieldType::FLOAT}, {"py", FieldType::FLOAT},
                         {"pz", FieldType::FLOAT}};

    // 3 ops: px = px+vx, py = py+vy, pz = pz+vz.
    def.ops.push_back({SystemOp::Kind::ADD, 0, 3, 0, 0, 0, 0});
    def.ops.push_back({SystemOp::Kind::ADD, 1, 4, 0, 0, 0, 1});
    def.ops.push_back({SystemOp::Kind::ADD, 2, 5, 0, 0, 0, 2});
    def.gpu_eligible = true;

    // 1) Transpilación a HLSL.
    ComputeCompiler compiler;
    ShaderArtifact art = compiler.compile(def);
    CHECK(art.name == "IntegrateVelocity");
    CHECK(art.hlsl.find("CSMain") != std::string::npos);
    CHECK(art.hlsl.find("RWStructuredBuffer") != std::string::npos);
    CHECK(art.hlsl.find("g_out[i*stride+0] = a3[0] + b3[0]") != std::string::npos);
    CHECK(art.registers == 9); // 6 in + 3 out

    // 2) Fallback CPU: MISMA definición ejecutada en CPU.
    // Entidad 0: pos(1,2,3) vel(10,20,30) → out(11,22,33).
    std::vector<float> data = {1,2,3, 10,20,30};
    CpuKernel cpu(def, &data);
    std::vector<float> scratch(6);
    cpu.run(scratch);
    CHECK(std::fabs(data[0] - 11.0f) < 1e-5f);
    CHECK(std::fabs(data[1] - 22.0f) < 1e-5f);
    CHECK(std::fabs(data[2] - 33.0f) < 1e-5f);
    CHECK(std::fabs(data[3] - 10.0f) < 1e-5f); // velocity sin tocar

    // 3) Heurística: por debajo del umbral → CPU.
    DeviceProfile weak = {true, 500, 10000};
    CHECK(!compiler.should_use_gpu(weak));
    DeviceProfile big = {true, 50000, 10000};
    CHECK(compiler.should_use_gpu(big));
    DeviceProfile no_gpu = {false, 50000, 10000};
    CHECK(!compiler.should_use_gpu(no_gpu));

    // 4) Ops con escalar: LERP / CLAMP.
    SystemDef damp;
    damp.name = "DampenVelocity";
    damp.input.fields = {{"v", FieldType::FLOAT}, {"target", FieldType::FLOAT}};
    damp.output.fields = {{"v", FieldType::FLOAT}};
    damp.ops.push_back({SystemOp::Kind::LERP, 0, 1, 0.5f, 0, 0, 0}); // v = v + 0.5*(target-v)
    damp.ops.push_back({SystemOp::Kind::CLAMP, 0, 0, 0, -5.0f, 5.0f, 0});
    ShaderArtifact art2 = compiler.compile(damp);
    CHECK(art2.hlsl.find("0.50000f") != std::string::npos);
    CHECK(art2.hlsl.find("clamp") != std::string::npos);

    std::vector<float> d2 = {10.0f, 0.0f};
    CpuKernel cpu2(damp, &d2);
    std::vector<float> sc2(2);
    cpu2.run(sc2);
    // LERP: 10 + 0.5*(0-10) = 5; luego CLAMP(5,-5,5) = 5.
    CHECK(std::fabs(d2[0] - 5.0f) < 1e-5f);

    // CLAMP con valor alto.
    std::vector<float> d3 = {100.0f, 0.0f};
    CpuKernel cpu3(damp, &d3);
    cpu3.run(sc2);
    CHECK(std::fabs(d3[0] - 5.0f) < 1e-5f);

    // Sistema NO gpu_eligible → solo CPU.
    SystemDef cpu_only = def;
    cpu_only.gpu_eligible = false;
    ShaderArtifact art3 = compiler.compile(cpu_only);
    CHECK(art3.hlsl.size() > 0); // sigue emitiendo (para info/debug)

    std::cout << "--- GPU COMPUTE SYSTEM COMPILATION TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}