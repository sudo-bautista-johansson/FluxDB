#include "../headers/executor.h"
#include "../headers/ecs.h"
#include "../headers/spatial_index.h"
#include <iostream>
#include <vector>

namespace veldradb {
namespace query {

void Executor::execute(const ExecutionPlan& plan) {
    if (!plan.root) {
        std::cout << "   ✗ Empty plan\n";
        return;
    }

    if (auto* s = dynamic_cast<const SelectPlanNode*>(plan.root.get())) {
        execute_select(s);
    } else if (auto* i = dynamic_cast<const InsertPlanNode*>(plan.root.get())) {
        execute_insert(i);
    } else if (auto* u = dynamic_cast<const UpdatePlanNode*>(plan.root.get())) {
        execute_update(u);
    } else if (auto* d = dynamic_cast<const DeletePlanNode*>(plan.root.get())) {
        execute_delete(d);
    } else if (auto* f = dynamic_cast<const FindPlanNode*>(plan.root.get())) {
        execute_find(f);
    } else if (auto* sp = dynamic_cast<const SpawnPlanNode*>(plan.root.get())) {
        execute_spawn(sp);
    } else if (auto* sn = dynamic_cast<const SnapshotPlanNode*>(plan.root.get())) {
        execute_snapshot(sn);
    } else if (auto* r = dynamic_cast<const RestorePlanNode*>(plan.root.get())) {
        execute_restore(r);
    } else {
        std::cout << "   ✗ Unknown plan node type\n";
    }
}

void Executor::execute_select(const SelectPlanNode* node) {
    std::cout << "   SELECT de tabla: " << node->table
              << " | scan: " << (node->scan_type == ScanType::FULL_SCAN ? "FULL" : "INDEX")
              << " | limit: " << node->limit << "\n";
}

void Executor::execute_insert(const InsertPlanNode* node) {
    std::cout << "   INSERT en tabla: " << node->table
              << " | columnas: " << node->columns.size() 
              << " | valores ingresados: " << node->values.size() << "\n";
}

void Executor::execute_update(const UpdatePlanNode* node) {
    std::cout << "   UPDATE en tabla: " << node->table
              << " | asignaciones: " << node->assignments.size() << "\n";
}

void Executor::execute_find(const FindPlanNode* node) {
    std::cout << "   FIND de tabla: " << node->table
              << " | scan: " << (node->scan_type == ScanType::SPATIAL_SCAN ? "SPATIAL" : "FULL")
              << " | near: " << (node->has_near ? "Si" : "No") << "\n";

    if (node->scan_type == ScanType::SPATIAL_SCAN && world_) {
        std::vector<veldradb::ecs::Entity> results;
        world_->get_spatial_index()->query_range(node->near_x, node->near_y, node->near_z, node->within, results);
        
        std::cout << "     -> [SPATIAL INDEX] Encontrados " << results.size() << " candidatos en el radio " << node->within << "\n";
        for (auto e : results) {
            std::cout << "        * Entity ID: " << e << "\n";
        }
    }
}

void Executor::execute_delete(const DeletePlanNode* node) {
    std::cout << "   DELETE de tabla: " << node->table << "\n";
}

void Executor::execute_spawn(const SpawnPlanNode* node) {
    std::cout << "   SPAWN prefab: " << node->prefab 
              << " | componentes: " << node->components.size() << "\n";
}

void Executor::execute_snapshot(const SnapshotPlanNode* node) {
    std::cout << "   SNAPSHOT a slot: " << node->slot << "\n";
}

void Executor::execute_restore(const RestorePlanNode* node) {
    std::cout << "   RESTORE desde slot: " << node->slot << "\n";
}

} // namespace query
} // namespace fluxdb
