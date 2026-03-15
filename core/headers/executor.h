#pragma once

#include "planner.h"
#include <string_view>
#include <memory>

namespace veldradb {
namespace ecs { class World; } // Forward declare in the correct nested namespace
namespace query {

// ─────────────────────────────────────────
//  Ejecutor
// ─────────────────────────────────────────

class Executor {
public:
    Executor(veldradb::ecs::World* world = nullptr) : world_(world) {}
    
    // Ejecuta el plan completo
    void execute(const ExecutionPlan& plan);

private:
    veldradb::ecs::World* world_;
    
    void execute_select(const SelectPlanNode* node);
    void execute_insert(const InsertPlanNode* node);
    void execute_update(const UpdatePlanNode* node);
    void execute_delete(const DeletePlanNode* node);
    void execute_find(const FindPlanNode* node);
    void execute_spawn(const SpawnPlanNode* node);
    void execute_snapshot(const SnapshotPlanNode* node);
    void execute_restore(const RestorePlanNode* node);
};

} // namespace query
} // namespace fluxdb
