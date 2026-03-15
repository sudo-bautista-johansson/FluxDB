#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ecs.h"

// Forward declaration of lua_State
struct lua_State;

namespace fluxdb {
namespace query {

class ScriptEngine {
public:
    ScriptEngine(veldradb::ecs::World* world);
    ~ScriptEngine();

    // Runs a raw Lua script
    bool run_script(const std::string& code);

    // Runs a script on all entities matching a GQL condition
    // e.g. "UPDATE players SET health = health + 10 WHERE distance(...) < 5"
    // can be modeled as ScriptEngine::run_conditional_script(lua_code, query_plan)
    bool run_conditional_script(const std::string& lua_code, const std::string& gql_condition);

private:
    void init_lua();
    void register_bindings();
    
    lua_State* L;
    veldradb::ecs::World* world_;
};

} // namespace query
} // namespace fluxdb
