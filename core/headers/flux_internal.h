#pragma once
#include <memory>
#include "storage.h"
#include "ecs.h"
#include "pubsub.h"
#include "vacuum.h"
#include "scripting.h"

struct FluxDB {
    std::shared_ptr<fluxdb::storage::LogManager> log_mgr;
    std::shared_ptr<fluxdb::storage::PageManager> disk_mgr;
    std::shared_ptr<fluxdb::storage::BufferPoolManager> buffer_pool;
    std::shared_ptr<fluxdb::storage::VacuumManager> vacuum_mgr;
    
    std::shared_ptr<fluxdb::ecs::ComponentStore> component_store;
    std::shared_ptr<fluxdb::ecs::HistoryManager> history_mgr;
    std::shared_ptr<fluxdb::query::SubscriptionManager> pubsub_mgr;
    std::shared_ptr<fluxdb::ecs::World> world;
    std::unique_ptr<fluxdb::query::ScriptEngine> script_engine;

    FluxDB();
};
