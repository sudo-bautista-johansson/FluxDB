#include "../headers/scheduler.h"
#include <algorithm>
#include <set>
#include <queue>
#include <numeric>

namespace fluxdb {
namespace det {

// ── SystemScheduler ──────────────────────────────────────────

size_t SystemScheduler::register_system(std::string name, SystemFn fn) {
    job_graph_dirty_ = true;
    size_t id = systems_.size();
    systems_.push_back({std::move(name), std::move(fn), ecs::TIER_MASK_ALL, {}, {}});
    return id;
}

void SystemScheduler::set_system_tier_access(size_t system_id, uint8_t tier_mask) {
    if (system_id < systems_.size()) {
        systems_[system_id].tier_mask = tier_mask;
    }
}

uint8_t SystemScheduler::system_tier_access(size_t system_id) const {
    if (system_id >= systems_.size()) return ecs::TIER_MASK_ALL;
    return systems_[system_id].tier_mask;
}

void SystemScheduler::set_system_access(size_t system_id, ComponentMask read_mask, ComponentMask write_mask) {
    if (system_id < systems_.size()) {
        systems_[system_id].read_mask = read_mask;
        systems_[system_id].write_mask = write_mask;
        job_graph_dirty_ = true;
    }
}

const ComponentMask& SystemScheduler::system_read_mask(size_t system_id) const {
    static ComponentMask empty;
    if (system_id >= systems_.size()) return empty;
    return systems_[system_id].read_mask;
}

const ComponentMask& SystemScheduler::system_write_mask(size_t system_id) const {
    static ComponentMask empty;
    if (system_id >= systems_.size()) return empty;
    return systems_[system_id].write_mask;
}

const std::vector<JobNode>& SystemScheduler::export_job_nodes() const {
    if (cached_job_nodes_.empty() || job_graph_dirty_) {
        cached_job_nodes_.clear();
        for (size_t i = 0; i < systems_.size(); ++i) {
            cached_job_nodes_.push_back({i, systems_[i].read_mask, systems_[i].write_mask});
        }
    }
    return cached_job_nodes_;
}

void SystemScheduler::rebuild_job_graph() const {
    if (!job_graph_dirty_) return;
    job_graph_.build(export_job_nodes());
    job_graph_dirty_ = false;
}

void SystemScheduler::step(ecs::World& world) {
    world.advance_tick();
    for (const System& s : systems_) {
        if (s.tier_mask != ecs::TIER_MASK_ALL) {
            prefetched_pages_ += world.prefetch_tiers(s.tier_mask);
        }
        s.fn(world);
    }
}

// ── JobGraph ─────────────────────────────────────────────────

void JobGraph::build(const std::vector<JobNode>& nodes) {
    nodes_ = nodes;
    layers_.clear();

    size_t n = nodes.size();
    if (n == 0) return;

    // Build adjacency: edge i→j if sys_i writes a component sys_j reads
    // or sys_i writes a component sys_j writes (write-write conflict).
    // Key insight: if sys_i writes comp X and sys_j reads comp X, they
    // have a dependency edge i→j (i must execute before j).
    std::vector<std::vector<size_t>> deps(n);
    std::vector<size_t> in_degree(n, 0);

    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            // Edge i→j if i writes something j reads OR i writes something j writes
            bool conflict = false;
            for (size_t c = 0; c < ecs::MAX_COMPONENTS && !conflict; ++c) {
                if (nodes[i].write_mask.test(c) && (nodes[j].read_mask.test(c) || nodes[j].write_mask.test(c))) {
                    conflict = true;
                }
            }
            if (conflict) {
                deps[i].push_back(j);
                in_degree[j]++;
            }
        }
    }

    // Topological sort via Kahn's algorithm. At each step, all nodes with
    // zero in-degree form a layer (they have no remaining dependencies and
    // can execute in parallel since their write sets are non-overlapping
    // within the same registration order).
    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) q.push(i);
    }

    std::vector<size_t> sorted;
    sorted.reserve(n);
    // Track the canonical maximum index processed per layer to break ties
    // deterministically — within a layer, smaller IDs execute first
    // (matches canonical registration order).

    while (!q.empty()) {
        ExecutionLayer layer;
        std::queue<size_t> next_q;
        std::vector<size_t> batch;
        while (!q.empty()) {
            size_t id = q.front(); q.pop();
            batch.push_back(id);
        }
        // Sort batch by system_id to maintain canonical order within layer.
        std::sort(batch.begin(), batch.end());
        for (size_t id : batch) {
            layer.system_ids.push_back(id);
            sorted.push_back(id);
            for (size_t dep : deps[id]) {
                if (--in_degree[dep] == 0) {
                    next_q.push(dep);
                }
            }
        }
        layers_.push_back(std::move(layer));
        q = std::move(next_q);
    }
}

std::vector<size_t> JobGraph::layer_sizes() const {
    std::vector<size_t> sizes;
    sizes.reserve(layers_.size());
    for (const auto& layer : layers_) {
        sizes.push_back(layer.system_ids.size());
    }
    return sizes;
}

size_t JobGraph::total_systems() const { return nodes_.size(); }

// ── Parallel execution ───────────────────────────────────────

void SystemScheduler::execute_layer(ecs::World& world, const ExecutionLayer& layer,
                                     size_t thread_idx, size_t num_threads) {
    // Distribute systems within this layer across threads in a
    // deterministic, round-robin fashion based on canonical order.
    for (size_t i = thread_idx; i < layer.system_ids.size(); i += num_threads) {
        size_t sys_id = layer.system_ids[i];
        const System& s = systems_[sys_id];
        if (s.tier_mask != ecs::TIER_MASK_ALL) {
            prefetched_pages_ += world.prefetch_tiers(s.tier_mask);
        }
        s.fn(world);
    }
}

void SystemScheduler::step_parallel(ecs::World& world, size_t num_threads) {
    rebuild_job_graph();

    world.advance_tick();

    if (num_threads <= 1 || job_graph_.layers().empty()) {
        // Fall back to sequential if no parallelism available.
        step(world);
        return;
    }

    for (const ExecutionLayer& layer : job_graph_.layers()) {
        if (layer.system_ids.size() <= 1) {
            // Single system: execute directly (no thread overhead).
            size_t sys_id = layer.system_ids[0];
            const System& s = systems_[sys_id];
            if (s.tier_mask != ecs::TIER_MASK_ALL) {
                prefetched_pages_ += world.prefetch_tiers(s.tier_mask);
            }
            s.fn(world);
            continue;
        }

        // Multi-system layer: parallel execution.
        size_t threads = std::min(num_threads, layer.system_ids.size());
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (size_t t = 0; t < threads; ++t) {
            workers.emplace_back(&SystemScheduler::execute_layer, this,
                                 std::ref(world), std::cref(layer), t, threads);
        }
        for (auto& t : workers) {
            t.join();
        }
    }
}

} // namespace det
} // namespace fluxdb