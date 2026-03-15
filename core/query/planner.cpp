// VeldraDB - Planner
// Takes an AST and produces an optimized execution plan

#include "../headers/planner.h"
#include <stdexcept>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────

Planner::Planner(std::string_view sql) : sql_(sql) {}

// ─────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────

ExecutionPlan Planner::plan() {
    Parser parser(sql_);
    auto ast = parser.parse();

    ExecutionPlan ep;
    ep.root = create_plan(ast.get());

    // Transferimos ownership del AST al plan
    // Los punteros crudos en los nodos apuntan al AST
    // que vive mientras el ExecutionPlan viva
    return ep;
}

// ─────────────────────────────────────────
//  Dispatcher — decide que plan crear
// ─────────────────────────────────────────

std::unique_ptr<PlanNode> Planner::create_plan(ASTNode* ast) {
    if (auto* s = dynamic_cast<SelectStatement*>(ast))
        return plan_select(s);
    if (auto* s = dynamic_cast<InsertStatement*>(ast))
        return plan_insert(s);
    if (auto* s = dynamic_cast<UpdateStatement*>(ast))
        return plan_update(s);
    if (auto* s = dynamic_cast<DeleteStatement*>(ast))
        return plan_delete(s);
    if (auto* s = dynamic_cast<FindStatement*>(ast))
        return plan_find(s);
    if (auto* s = dynamic_cast<SpawnStatement*>(ast))
        return plan_spawn(s);
    if (auto* s = dynamic_cast<SnapshotStatement*>(ast))
        return plan_snapshot(s);
    if (auto* s = dynamic_cast<RestoreStatement*>(ast))
        return plan_restore(s);

    throw std::runtime_error("Planner: tipo de statement desconocido");
}

// ─────────────────────────────────────────
//  Optimizador de scan
// ─────────────────────────────────────────

// Por ahora siempre full scan
// Cuando tengamos indices, aca decidiremos cual usar
ScanType Planner::choose_scan(const std::string& table, ASTNode* filter) {
    std::string best_idx = find_best_index(filter);
    
    // Si la condición de filtrado usa componentes espaciales conocidos, usamos indice
    if (best_idx == "pos" || best_idx == "transform" || best_idx == "location") {
        return ScanType::SPATIAL_SCAN; // Route to R-Tree / Grid
    }
    
    if (!best_idx.empty()) {
        return ScanType::INDEX_SCAN; // B-Tree / Hash
    }

    return ScanType::FULL_SCAN;
}

std::string Planner::find_best_index(ASTNode* filter) {
    // Busca el primer BinaryExpr con EQ que tenga un IDENT a la izquierda
    // Esa columna es la mejor candidata para usar un indice
    if (auto* bin = dynamic_cast<BinaryExpr*>(filter)) {
        if (bin->op.type == TokenType::EQ || bin->op.type == TokenType::LT || bin->op.type == TokenType::GT) {
            if (auto* ident = dynamic_cast<IdentExpr*>(bin->left.get())) {
                return ident->name; // ej: "tag" o "pos"
            }
        }
        
        // Buscar recursivamente en AND y OR
        if (bin->op.type == TokenType::AND || bin->op.type == TokenType::OR) {
            std::string left_idx  = find_best_index(bin->left.get());
            if (!left_idx.empty()) return left_idx;
            return find_best_index(bin->right.get());
        }
        
        // Función distance()
        if (auto* func = dynamic_cast<CallExpr*>(bin->left.get())) {
            if (func->function_name == "distance") {
                return "pos"; // Force spatial scan if distance() is used
            }
        }
    }
    return ""; // sin indice
}

// ─────────────────────────────────────────
//  Planes concretos
// ─────────────────────────────────────────

std::unique_ptr<PlanNode> Planner::plan_select(SelectStatement* stmt) {
    auto node = std::make_unique<SelectPlanNode>();
    node->table   = stmt->table;
    node->columns = stmt->columns;
    node->limit   = stmt->limit   ? stmt->limit->value   : -1;
    node->filter  = stmt->where   ? stmt->where->condition.get() : nullptr;
    node->at_tick = stmt->at_tick;

    node->scan_type = choose_scan(stmt->table, node->filter);

    if (node->scan_type == ScanType::INDEX_SCAN)
        node->index_column = find_best_index(node->filter);

    return node;
}

std::unique_ptr<PlanNode> Planner::plan_insert(InsertStatement* stmt) {
    auto node = std::make_unique<InsertPlanNode>();
    node->table   = stmt->table;
    node->columns = stmt->columns;
    for (auto& v : stmt->values)
        node->values.push_back(v.get());
    return node;
}

std::unique_ptr<PlanNode> Planner::plan_update(UpdateStatement* stmt) {
    auto node = std::make_unique<UpdatePlanNode>();
    node->table  = stmt->table;
    node->filter = stmt->where ? stmt->where->condition.get() : nullptr;
    for (auto& [col, val] : stmt->assignments)
        node->assignments.emplace_back(col, val.get());
    return node;
}

std::unique_ptr<PlanNode> Planner::plan_delete(DeleteStatement* stmt) {
    auto node = std::make_unique<DeletePlanNode>();
    node->table  = stmt->table;
    node->filter = stmt->where ? stmt->where->condition.get() : nullptr;
    return node;
}

std::unique_ptr<PlanNode> Planner::plan_find(FindStatement* stmt) {
    auto node = std::make_unique<FindPlanNode>();
    node->table   = stmt->table;
    node->limit   = stmt->limit ? stmt->limit->value : -1;
    node->filter  = stmt->where ? stmt->where->condition.get() : nullptr;

    if (stmt->has_near) {
        node->has_near  = true;
        node->near_x    = stmt->near_x;
        node->near_y    = stmt->near_y;
        node->near_z    = stmt->near_z;
        node->within    = stmt->within;
        node->scan_type = ScanType::SPATIAL_SCAN;
    } else {
        node->scan_type = choose_scan(stmt->table, node->filter);
    }

    return node;
}

std::unique_ptr<PlanNode> Planner::plan_spawn(SpawnStatement* stmt) {
    auto node = std::make_unique<SpawnPlanNode>();
    node->prefab = stmt->prefab;
    for (auto& [key, val] : stmt->components)
        node->components.emplace_back(key, val.get());
    return node;
}

std::unique_ptr<PlanNode> Planner::plan_snapshot(SnapshotStatement* stmt) {
    auto node = std::make_unique<SnapshotPlanNode>();
    node->slot = stmt->slot;
    return node;
}

std::unique_ptr<PlanNode> Planner::plan_restore(RestoreStatement* stmt) {
    auto node = std::make_unique<RestorePlanNode>();
    node->slot = stmt->slot;
    return node;
}

} // namespace query
} // namespace veldradb