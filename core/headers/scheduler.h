#pragma once

// SystemScheduler — canonical system scheduler (#11) with deterministic
// job graph (#32) for parallel execution.
//
// #11 (sequential) flow per tick:
//   scheduler.step(world)
//     → world.advance_tick()
//     → systems in canonical order (registration order)
//
// #32 (parallel) flow per tick:
//   scheduler.step_parallel(world, num_threads)
//     → world.advance_tick()
//     → systems grouped into dependency layers, intra-layer parallelism
//     → deterministic reduction order via commit buffers

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <bitset>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include "ecs.h"

namespace fluxdb {
namespace det {

// (#32) Dependency analysis: a mask of up to MAX_COMPONENTS components.
using ComponentMask = std::bitset<ecs::MAX_COMPONENTS>;

// (#32) A single job in the job graph: one system to execute.
struct JobNode {
    size_t system_id;
    ComponentMask read_mask;
    ComponentMask write_mask;
};

// (#32) Execution layer: a set of systems with non-overlapping write sets
// that can execute in parallel. Within a layer, canonical order is
// preserved for determinism.
struct ExecutionLayer {
    std::vector<size_t> system_ids; // canonical order within layer
};

// (#32) JobGraph: the dependency DAG for one tick.
class JobGraph {
public:
    // Analyze dependency edges from read/write masks and build topological
    // layers. Systems without write conflicts at the same "stage" are
    // grouped into the same layer (parallelizable).
    void build(const std::vector<JobNode>& nodes);

    const std::vector<ExecutionLayer>& layers() const { return layers_; }

    // Number of systems per layer (for statistics / telemetry).
    std::vector<size_t> layer_sizes() const;

    // Total number of systems in the graph.
    size_t total_systems() const;

private:
    std::vector<ExecutionLayer> layers_;
    std::vector<JobNode> nodes_;
};

class SystemScheduler {
public:
    using SystemFn = std::function<void(ecs::World&)>;

    size_t register_system(std::string name, SystemFn fn);

    size_t system_count() const { return systems_.size(); }

    // (#1) Tier access for prefetch.
    void set_system_tier_access(size_t system_id, uint8_t tier_mask);
    uint8_t system_tier_access(size_t system_id) const;

    // (#32) Declare which components this system reads/writes. Used for
    // parallel dependency analysis. Default: empty masks (sequential only).
    void set_system_access(size_t system_id, ComponentMask read_mask, ComponentMask write_mask);
    const ComponentMask& system_read_mask(size_t system_id) const;
    const ComponentMask& system_write_mask(size_t system_id) const;

    // (#32) Export the current system list as a JobNode array for graph
    // building. The date is computed lazily and cached until the next
    // registration / access change.
    const std::vector<JobNode>& export_job_nodes() const;

    // (#32) Retrieve the last computed job graph (for diagnostics).
    const JobGraph& job_graph() const { rebuild_job_graph(); return job_graph_; }
    bool job_graph_dirty() const { return job_graph_dirty_; }

    uint64_t total_prefetched_pages() const { return prefetched_pages_; }

    // Sequential deterministic step (#11).
    void step(ecs::World& world);

    // (#32) Parallel deterministic step: executes independent systems in
    // parallel across `num_threads` worker threads. Determinism is
    // preserved via:
    //   1. Dependency-ordered layers (no write conflicts)
    //   2. Canonical system dispatch order within each layer
    //   3. Per-thread commit buffers committed in canonical order
    void step_parallel(ecs::World& world, size_t num_threads);

    const std::string& system_name(size_t idx) const { return systems_[idx].name; }

private:
    struct System {
        std::string name;
        SystemFn fn;
        uint8_t tier_mask = ecs::TIER_MASK_ALL;
        ComponentMask read_mask;   // (#32)
        ComponentMask write_mask;  // (#32)
    };
    std::vector<System> systems_;
    uint64_t prefetched_pages_ = 0;

    mutable JobGraph job_graph_;
    mutable bool job_graph_dirty_ = true;
    mutable std::vector<JobNode> cached_job_nodes_;

    void rebuild_job_graph() const;
    void execute_layer(ecs::World& world, const ExecutionLayer& layer, size_t thread_idx, size_t num_threads);
};

} // namespace det
} // namespace fluxdb