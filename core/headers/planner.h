#pragma once
#include "parser.h"
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <cstdint>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Tipos de scan
// ─────────────────────────────────────────

enum class ScanType {
    FULL_SCAN,      // leer toda la tabla
    INDEX_SCAN,     // usar un indice
    SPATIAL_SCAN,   // usar R-Tree para NEAR
};

// ─────────────────────────────────────────
//  Nodos del plan
// ─────────────────────────────────────────

struct PlanNode {
    virtual ~PlanNode() = default;
};

struct SelectPlanNode : PlanNode {
    std::string table;
    std::vector<std::string> columns;   // ["*"] o ["id", "name"]
    ScanType scan_type = ScanType::FULL_SCAN;
    std::string index_column;           // si es INDEX_SCAN
    ASTNode* filter = nullptr;          // condicion WHERE (no owner)
    int limit = -1;
    uint64_t at_tick = UINT64_MAX;
};

struct InsertPlanNode : PlanNode {
    std::string table;
    std::vector<std::string> columns;
    std::vector<ASTNode*> values;       // no owner
};

struct UpdatePlanNode : PlanNode {
    std::string table;
    std::vector<std::pair<std::string, ASTNode*>> assignments; // no owner
    ASTNode* filter = nullptr;
};

struct DeletePlanNode : PlanNode {
    std::string table;
    ASTNode* filter = nullptr;
};

struct FindPlanNode : PlanNode {
    std::string table;
    ScanType scan_type = ScanType::FULL_SCAN;
    ASTNode* filter = nullptr;
    int limit = -1;
    // Espacial
    bool has_near = false;
    float near_x = 0, near_y = 0, near_z = 0;
    float within  = 0;
};

struct SpawnPlanNode : PlanNode {
    std::string prefab;
    std::vector<std::pair<std::string, ASTNode*>> components; // no owner
};

struct SnapshotPlanNode : PlanNode {
    std::string slot;
};

struct RestorePlanNode : PlanNode {
    std::string slot;
};

// ─────────────────────────────────────────
//  Plan de ejecucion completo
// ─────────────────────────────────────────

struct ExecutionPlan {
    std::unique_ptr<PlanNode> root;     // un query = un plan
};

// ─────────────────────────────────────────
//  Planner
// ─────────────────────────────────────────

class Planner {
public:
    explicit Planner(std::string_view sql);
    ExecutionPlan plan();

private:
    std::string sql_;
    
    std::unique_ptr<PlanNode> create_plan(ASTNode* ast);
    std::unique_ptr<PlanNode> plan_select(SelectStatement* stmt);
    std::unique_ptr<PlanNode> plan_insert(InsertStatement* stmt);
    std::unique_ptr<PlanNode> plan_update(UpdateStatement* stmt);
    std::unique_ptr<PlanNode> plan_delete(DeleteStatement* stmt);
    std::unique_ptr<PlanNode> plan_find(FindStatement* stmt);
    std::unique_ptr<PlanNode> plan_spawn(SpawnStatement* stmt);
    std::unique_ptr<PlanNode> plan_snapshot(SnapshotStatement* stmt);
    std::unique_ptr<PlanNode> plan_restore(RestoreStatement* stmt);

    // Optimizaciones
    ScanType choose_scan(const std::string& table, ASTNode* filter);
    std::string find_best_index(ASTNode* filter);
};

} // namespace query
} // namespace veldradb