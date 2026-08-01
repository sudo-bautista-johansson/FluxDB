// FluxDB — Feature #34: Causal Divergence Tracing ("Why Did We Desync")
// Detección de desync por checksums de chunk + trace-back causal hasta
// el primer write divergente y la cadena de inputs que lo produjo.
#include "../core/headers/causal_divergence.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace fluxdb::div;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB Causal Divergence Tracing Test (#34) ---\n";

    // Cadena causal correcta (máquina A):
    //   tick1 BuffAggregator escribe ActiveBuffs=[Shield] (1 buff)
    //   tick2 BuffAggregator lee ActiveBuffs → escribe Armor=42
    //   tick3 DamageResolution lee Armor → escribe Health=58
    Replica machineA;
    machineA.record_write(1, "BuffAggregator", 10, "ActiveBuffs", 0, 1, "", 0);
    machineA.record_write(2, "BuffAggregator", 10, "Armor", 40, 42, "ActiveBuffs", 1);
    machineA.record_write(3, "DamageResolution", 10, "Health", 100, 58, "Armor", 42);

    // Máquina B: idéntica pero el tick 1 difiere (el buff agregó [Regen]):
    //   tick1 ActiveBuffs=[Shield,Regen] (2 buffs) → Armor=40 → Health=60
    Replica machineB;
    machineB.record_write(1, "BuffAggregator", 10, "ActiveBuffs", 0, 2, "", 0);
    machineB.record_write(2, "BuffAggregator", 10, "Armor", 40, 40, "ActiveBuffs", 2);
    machineB.record_write(3, "DamageResolution", 10, "Health", 100, 60, "Armor", 40);

    CausalDivergenceTracer tracer;

    // 1) Detección: el primer tick divergente es el 1 (chunk ActiveBuffs).
    DivergenceReport rep = tracer.find_first_divergence(machineA, machineB);
    CHECK(rep.diverged);
    CHECK(rep.tick == 1);
    CHECK(rep.component_chunk == "ActiveBuffs");

    // 2) Trace-back: la cadena causal llega a BuffAggregator @ tick 1.
    CausalChain chain = tracer.trace(machineA, machineB);
    CHECK(chain.found);
    CHECK(chain.first_divergent_tick == 1);
    CHECK(!chain.links.empty());
    CHECK(chain.links.back().system == "BuffAggregator");
    CHECK(chain.links.back().tick == 1);
    CHECK(chain.links.back().component == "ActiveBuffs");
    // El valor del input divergente es 1 vs 2.
    CHECK(chain.links.back().local_value == 1);
    CHECK(chain.links.back().remote_value == 2);
    CHECK(chain.links.back().value_diverges);

    // La cadena tiene 3 eslabones: Health(3) ← Armor(2) ← ActiveBuffs(1).
    CHECK(chain.links.size() == 3);
    CHECK(chain.links[0].system == "DamageResolution");
    CHECK(chain.links[0].input_read == "Armor");
    CHECK(chain.links[1].system == "BuffAggregator");
    CHECK(chain.links[1].tick == 2);
    CHECK(chain.links[1].input_read == "ActiveBuffs");
    CHECK(chain.root_cause.find("BuffAggregator @ tick 1") != std::string::npos);

    // 3) Dos réplicas idénticas → sin divergencia.
    Replica cloneA;
    cloneA.record_write(1, "BuffAggregator", 10, "ActiveBuffs", 0, 1, "", 0);
    cloneA.record_write(2, "BuffAggregator", 10, "Armor", 40, 42, "ActiveBuffs", 1);
    cloneA.record_write(3, "DamageResolution", 10, "Health", 100, 58, "Armor", 42);
    DivergenceReport rep2 = tracer.find_first_divergence(machineA, cloneA);
    CHECK(!rep2.diverged);

    // 4) Divergencia posterior (tick 3): divergencia en Health.
    Replica machineC;
    machineC.record_write(1, "BuffAggregator", 10, "ActiveBuffs", 0, 1, "", 0);
    machineC.record_write(2, "BuffAggregator", 10, "Armor", 40, 42, "ActiveBuffs", 1);
    machineC.record_write(3, "DamageResolution", 10, "Health", 100, 50, "Armor", 42);
    DivergenceReport rep3 = tracer.find_first_divergence(machineA, machineC);
    CHECK(rep3.diverged);
    CHECK(rep3.tick == 3);
    CHECK(rep3.component_chunk == "Health");

    // 5) Nombres de sistema hash = mismos → trace estable.
    Replica big_a, big_b;
    for (int t = 1; t <= 100; ++t) {
        big_a.record_write(t, "MotionSystem", 5, "Position", t - 1, t, "Velocity", 1);
        big_b.record_write(t, "MotionSystem", 5, "Position", t - 1, t, "Velocity", 1);
    }
    // Divergencia a partir del tick 60 (un write malo).
    big_b.record_write(60, "MotionSystem", 5, "Position", 59, 61, "Velocity", 2);
    CHECK(big_a.write_count() == 100);
    CHECK(big_b.write_count() == 101);
    DivergenceReport rep4 = tracer.find_first_divergence(big_a, big_b);
    CHECK(rep4.diverged);
    // El write malo se anexa fuera de orden; la checksum detecta la
    // divergencia en cuanto el chunk tocado se compara (>= 60).
    CHECK(rep4.tick >= 60);

    std::cout << "--- CAUSAL DIVERGENCE TRACING TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}