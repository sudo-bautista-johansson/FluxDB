#pragma once

#include "planner.h"
#include "ecs.h"
#include <string_view>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Ejecutor
// ─────────────────────────────────────────

class Executor {
public:
    Executor(fluxdb::ecs::World* world = nullptr) : world_(world) {}
    
    // Ejecuta el plan completo
    void execute(const ExecutionPlan& plan);

    // Compiled Query Plans (#5): reutilización del caché desde la capa SQL.
    // Registra la firma de componentes que representa una tabla lógica.
    void register_table(const std::string& name, std::vector<fluxdb::ecs::ComponentID> components);

    // Compila (y cachea) el plan de la tabla vía World::create_query (dedupe
    // por firma). QUERY_INVALID si la tabla no está registrada o no hay World.
    static constexpr fluxdb::ecs::QueryHandle QUERY_INVALID = UINT32_MAX;
    fluxdb::ecs::QueryHandle query_handle(const std::string& name) const;

    // Tablas registradas (lectura para tooling).
    const std::unordered_map<std::string, std::vector<fluxdb::ecs::ComponentID>>& tables() const {
        return table_components_;
    }

private:
    fluxdb::ecs::World* world_;
    std::unordered_map<std::string, std::vector<fluxdb::ecs::ComponentID>> table_components_;
    
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
