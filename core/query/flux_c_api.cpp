#include "../headers/flux_c_api.h"
#include "../headers/planner.h"
#include "../headers/executor.h"
#include "../headers/vacuum.h"
#include <string>
#include <iostream>
#include <sstream>
#include <cstring>

#include <memory>
#include "../headers/storage.h"
#include "../headers/ecs.h"
#include "../headers/pubsub.h"
#include "../headers/network.h"
#include "../headers/scripting.h"

#include "../headers/flux_internal.h"

FluxDB::FluxDB() {
    // Initialize Storage Subsystem
    log_mgr = std::make_shared<fluxdb::storage::LogManager>("fluxdb.log");
    disk_mgr = std::make_shared<fluxdb::storage::PageManager>("fluxdb.db");
    buffer_pool = std::make_shared<fluxdb::storage::BufferPoolManager>(1024, disk_mgr, log_mgr);
    
    // Initialize ECS Subsystem
    component_store = std::make_shared<fluxdb::ecs::ComponentStore>();
    history_mgr = std::make_shared<fluxdb::ecs::HistoryManager>(600);
    pubsub_mgr = std::make_shared<fluxdb::query::SubscriptionManager>();
    world = std::make_shared<fluxdb::ecs::World>(component_store, history_mgr, pubsub_mgr);

    // Initialize Scripting
    script_engine = std::make_unique<fluxdb::query::ScriptEngine>(world.get());
    
    // Run Crash Recovery
    fluxdb::storage::RecoveryManager recovery(log_mgr, buffer_pool.get());
    recovery.recover();

    // Boot Vacuum thread
    vacuum_mgr = std::make_shared<fluxdb::storage::VacuumManager>(disk_mgr, buffer_pool);
    vacuum_mgr->start(5); // run every 5s logic tick
}


struct FluxResult {
    std::string text_output;
};

// Global thread-local error message
static thread_local std::string g_last_error;

// Helper to capture std::cout output from Executor
class RedirectCout {
    std::streambuf* old_buf;
    std::ostringstream ss;
public:
    RedirectCout() {
        old_buf = std::cout.rdbuf(ss.rdbuf());
    }
    ~RedirectCout() {
        std::cout.rdbuf(old_buf);
    }
    std::string get_string() const { return ss.str(); }
};

extern "C" {

FLUX_API FluxDB* flux_init() {
    try {
        return new FluxDB();
    } catch(const std::exception& e) {
        g_last_error = e.what();
        return nullptr;
    }
}

FLUX_API FluxResult* flux_query(FluxDB* db, const char* sql) {
    if (!db || !sql) {
        g_last_error = "Invalid arguments to flux_query";
        return nullptr;
    }

    try {
        fluxdb::query::Planner planner(sql);
        auto plan = planner.plan();
        
        // We capture cout to return the executor's output string to the wrapper.
        RedirectCout rc;
        fluxdb::query::Executor executor(db->world.get());
        executor.execute(plan);

        FluxResult* res = new FluxResult();
        res->text_output = rc.get_string();
        return res;
        
    } catch(const std::exception& e) {
        g_last_error = e.what();
        return nullptr;
    }
}

FLUX_API const char* flux_result_get_text(FluxResult* result) {
    if (!result) return "";
    return result->text_output.c_str();
}

FLUX_API void flux_free_result(FluxResult* result) {
    if (result) {
        delete result;
    }
}

FLUX_API void flux_close(FluxDB* db) {
    if (db) {
        if (db->vacuum_mgr) {
            db->vacuum_mgr->stop();
        }
        delete db;
    }
}

FLUX_API const char* flux_get_last_error() {
    return g_last_error.c_str();
}

FLUX_API void flux_advance_tick(FluxDB* db) {
    if (db && db->history_mgr) {
        db->history_mgr->advance_tick();
    }
}

FLUX_API uint64_t flux_get_current_tick(FluxDB* db) {
    if (db && db->history_mgr) {
        return db->history_mgr->get_current_tick();
    }
    return 0;
}

FLUX_API size_t flux_get_delta_payload(FluxDB* db, uint64_t last_ack_tick, void* out_buffer, size_t buffer_size) {
    if (!db || !db->world || !out_buffer) return 0;
    
    try {
        fluxdb::network::DeltaCompression delta(db->world.get());
        std::vector<uint8_t> payload;
        delta.generate_delta_payload(last_ack_tick, payload);
        
        size_t to_copy = (payload.size() > buffer_size) ? buffer_size : payload.size();
        std::memcpy(out_buffer, payload.data(), to_copy);
        return to_copy;
    } catch (const std::exception& e) {
        g_last_error = e.what();
        return 0;
    }
}

FLUX_API bool flux_run_script(FluxDB* db, const char* lua_code) {
    if (!db || !db->script_engine || !lua_code) return false;
    return db->script_engine->run_script(lua_code);
}

} // extern "C"
