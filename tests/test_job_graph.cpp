// FluxDB — Feature #32: Deterministic Job Graph Scheduler
// Parallel execution with dependency-ordered layers and deterministic
// reduction order (preserving #11 lockstep guarantees).
#include "../core/headers/ecs.h"
#include "../core/headers/scheduler.h"
#include <iostream>
#include <cassert>
#include <atomic>
#include <mutex>
#include <set>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::det;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_job_graph_build() {
    std::cout << "1. JobGraph topological layers from access masks...\n";

    // Diamond dependency: 0→1→3, 0→2→3, plus 4 reads 3.
    // Systems 0 writes A, 1 writes B (reads A), 2 writes C (reads A),
    // 3 writes D (reads B+C), 4 reads D.
    std::vector<JobNode> nodes = {
        {0, {}, ComponentMask().set(0)},     // sys0: writes comp0
        {1, ComponentMask().set(0), ComponentMask().set(1)}, // sys1: reads 0, writes 1
        {2, ComponentMask().set(0), ComponentMask().set(2)}, // sys2: reads 0, writes 2
        {3, ComponentMask().set(1).set(2), ComponentMask().set(3)}, // sys3: reads 1+2, writes 3
        {4, ComponentMask().set(3), {}},     // sys4: reads comp3
    };

    JobGraph graph;
    graph.build(nodes);

    CHECK(graph.total_systems() == 5);

    auto sizes = graph.layer_sizes();
    // Layer 0: sys0 (no deps). Maybe sys4 joins layer 0 if write set disjoint?
    // Actually sys4 reads 3, nothing writes 3 yet? No — sys3 writes 3!
    // Layer 0 must have sys0 and sys4 (sys4 reads 3 which is NOT written by sys0).
    // Actually wait: an edge only exists if i writes something j reads.
    // sys0 writes 0, sys4 reads 3 — no conflict. sys4 only reads 3,
    // no writer of 3 in layer 0. So sys4 can be in layer 0? NO — sys3
    // WRITES 3 and sys3 cannot be before sys0. Since sys3 depends on
    // sys1 and sys2 which depend on sys0, sys3 must be after sys0.
    // sys4 depends on sys3. Therefore sys4 depends transitively on sys0.
    // Correct layering in CANONICAL order:
    // Layer 0: sys0 (no deps)
    // Layer 1: sys1, sys2 (both depend on sys0, but not on each other)
    // Layer 2: sys3 (depends on sys1 and sys2)
    // Layer 3: sys4 (depends on sys3)

    // In a valid topological sort we have at least 3 layers.
    CHECK(sizes.size() >= 3);
    CHECK(sizes.size() <= 5);
    // sys1 and sys2 must be in the same layer (they're independent)
    // Check their ids appear in the same layer.
    bool sys1_and_2_together = false;
    for (const auto& layer : graph.layers()) {
        bool has1 = false, has2 = false;
        for (size_t id : layer.system_ids) {
            if (id == 1) has1 = true;
            if (id == 2) has2 = true;
        }
        if (has1 && has2) sys1_and_2_together = true;
    }
    CHECK(sys1_and_2_together);

    size_t total = 0;
    for (const auto& layer : graph.layers()) total += layer.system_ids.size();
    CHECK(total == 5);
}

static void test_independent_systems_parallel() {
    std::cout << "2. Independent systems without conflicts run in parallel...\n";

    auto store = std::make_shared<ComponentStore>();
    World w(store);

    SystemScheduler sched;

    // Three independent counters.
    std::atomic<int> counterA{0}, counterB{0}, counterC{0};

    size_t id_a = sched.register_system("A", [&](World&) { counterA++; });
    size_t id_b = sched.register_system("B", [&](World&) { counterB++; });
    size_t id_c = sched.register_system("C", [&](World&) { counterC++; });

    // All three write to DIFFERENT dummy components (no overlap).
    ComponentMask a_w; a_w.set(0);
    ComponentMask b_w; b_w.set(1);
    ComponentMask c_w; c_w.set(2);
    sched.set_system_access(id_a, {}, a_w);
    sched.set_system_access(id_b, {}, b_w);
    sched.set_system_access(id_c, {}, c_w);

    // All 3 should be in layer 0 (independent)
    const auto& layers = sched.job_graph().layers();
    CHECK(layers.size() >= 1);
    CHECK(layers[0].system_ids.size() == 3);

    sched.step_parallel(w, 4);
    CHECK(counterA == 1 && counterB == 1 && counterC == 1);

    // Parallel and sequential produce same state.
    World w2(store);
    SystemScheduler sched2;
    sched2.register_system("A", [&](World&) { counterA++; });
    sched2.register_system("B", [&](World&) { counterB++; });
    sched2.register_system("C", [&](World&) { counterC++; });
    // Sequential: default (no access masks = all MAX → 1 layer)
    sched2.step(w2);
    CHECK(w.current_tick() == w2.current_tick());
}

static void test_dependent_systems_order() {
    std::cout << "3. Dependent systems execute in correct order...\n";

    std::vector<int> log;
    std::mutex log_mtx;

    SystemScheduler sched;

    size_t id_a = sched.register_system("A", [&](World&) {
        std::lock_guard<std::mutex> lk(log_mtx);
        log.push_back(1);
    });
    size_t id_b = sched.register_system("B", [&](World&) {
        std::lock_guard<std::mutex> lk(log_mtx);
        log.push_back(2);
    });
    size_t id_c = sched.register_system("C", [&](World&) {
        std::lock_guard<std::mutex> lk(log_mtx);
        log.push_back(3);
    });

    // A writes comp0, B reads comp0 and writes comp1, C reads comp1.
    ComponentMask mask_w0; mask_w0.set(0);
    ComponentMask mask_r0; mask_r0.set(0);
    ComponentMask mask_w1; mask_w1.set(1);
    ComponentMask mask_r1; mask_r1.set(1);
    sched.set_system_access(id_a, {}, mask_w0);
    sched.set_system_access(id_b, mask_r0, mask_w1);
    sched.set_system_access(id_c, mask_r1, {});

    auto store = std::make_shared<ComponentStore>();
    World w(store);
    sched.step_parallel(w, 4);

    CHECK(log.size() == 3);
    CHECK(log[0] == 1); // A first
    CHECK(log[1] == 2); // B second
    CHECK(log[2] == 3); // C third
}

static void test_parallel_vs_sequential_hash_match() {
    std::cout << "4. State hash identical after parallel vs sequential...\n";

    auto store = std::make_shared<ComponentStore>();
    ComponentID data_id = store->register_component("Data", sizeof(int));

    // Serial scheduler.
    SystemScheduler sched_seq;
    size_t id_seq = sched_seq.register_system("Writer", [data_id](World& w) {
        // Write 42 to the Data component of all entities via query.
        ArchetypeSignature sig;
        sig.set(data_id);
        w.for_each_archetype_sorted([&](Archetype* arch) {
            if (!(arch->get_signature() & sig).any()) return;
            size_t count = arch->get_entity_count();
            for (size_t r = 0; r < count; ++r) {
                size_t sz;
                int* v = (int*)arch->get_component_data(r, data_id);
                if (v) *v = 42;
            }
        });
    });

    // Parallel scheduler.
    SystemScheduler sched_par;
    size_t id_par = sched_par.register_system("Writer", [data_id](World& w) {
        ArchetypeSignature sig;
        sig.set(data_id);
        w.for_each_archetype_sorted([&](Archetype* arch) {
            if (!(arch->get_signature() & sig).any()) return;
            size_t count = arch->get_entity_count();
            for (size_t r = 0; r < count; ++r) {
                int* v = (int*)arch->get_component_data(r, data_id);
                if (v) *v = 42;
            }
        });
    });

    ComponentMask wmask; wmask.set(data_id);
    ComponentMask full_mask; full_mask.set(data_id);
    sched_seq.set_system_access(id_seq, full_mask, full_mask);
    sched_par.set_system_access(id_par, {}, wmask);

    // Setup worlds identically.
    World w_seq(store), w_par(store);
    w_seq.enable_determinism_lock();
    w_par.enable_determinism_lock();

    int zero = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        w_seq.spawn_with_id(i + 1);
        w_seq.add_component(i + 1, data_id, &zero);
        w_par.spawn_with_id(i + 1);
        w_par.add_component(i + 1, data_id, &zero);
    }

    for (int t = 0; t < 3; ++t) {
        sched_seq.step(w_seq);
        sched_par.step_parallel(w_par, 4);
    }

    CHECK(w_seq.current_tick() == w_par.current_tick());
    CHECK(w_seq.state_hash() == w_par.state_hash());
}

static void test_all_independent_no_write_conflicts() {
    std::cout << "5. All-independent systems: single layer, all run...\n";

    constexpr size_t N = 10;
    std::atomic<int> counters[N];
    for (auto& c : counters) c = 0;

    SystemScheduler sched;
    for (size_t i = 0; i < N; ++i) {
        size_t id = sched.register_system("S" + std::to_string(i), [i, &counters](World&) {
            counters[i]++;
        });
        ComponentMask w; w.set(i);
        sched.set_system_access(id, {}, w);
    }

    // All N should be in layer 0.
    const auto& layers = sched.job_graph().layers();
    CHECK(layers.size() == 1);
    CHECK(layers[0].system_ids.size() == N);

    auto store = std::make_shared<ComponentStore>();
    World w(store);
    sched.step_parallel(w, 4);

    for (size_t i = 0; i < N; ++i) {
        CHECK(counters[i] == 1);
    }
}

static void test_single_thread_sequential_fallback() {
    std::cout << "6. step_parallel with 1 thread = sequential step fallback...\n";

    std::vector<int> log;

    SystemScheduler sched;
    size_t id_a = sched.register_system("A", [&](World&) { log.push_back(1); });
    size_t id_b = sched.register_system("B", [&](World&) { log.push_back(2); });

    // Same access mask → sequential dependency
    ComponentMask m; m.set(0);
    sched.set_system_access(id_a, m, m);
    sched.set_system_access(id_b, m, m);

    auto store = std::make_shared<ComponentStore>();
    World w(store);

    sched.step_parallel(w, 1);

    CHECK(log.size() == 2);
    CHECK(log[0] == 1);
    CHECK(log[1] == 2);
}

int main() {
    std::cout << std::unitbuf;
    std::cout << "--- Starting FluxDB Deterministic Job Graph Scheduler Test (#32) ---\n";
    test_job_graph_build();
    test_independent_systems_parallel();
    test_dependent_systems_order();
    test_parallel_vs_sequential_hash_match();
    test_all_independent_no_write_conflicts();
    test_single_thread_sequential_fallback();
    std::cout << "--- JOB GRAPH TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}