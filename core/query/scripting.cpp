#include "../headers/scripting.h"
#include "../headers/ecs.h"
#include "../headers/planner.h"
#include "../headers/executor.h"
#include <iostream>
extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace fluxdb {
namespace query {

// Helper to get World from Lua state (via upvalue)
static fluxdb::ecs::World* GetWorld(lua_State* L) {
    return static_cast<fluxdb::ecs::World*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Lua binding: flux.spawn({})
static int Lua_FluxSpawn(lua_State* L) {
    auto world = GetWorld(L);
    fluxdb::ecs::Entity ent = world->spawn();
    lua_pushinteger(L, ent);
    return 1;
}

// Lua binding: flux.get(entity, component_name)
static int Lua_FluxGet(lua_State* L) {
    auto world = GetWorld(L);
    uint32_t entity = (uint32_t)luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);

    auto store = world->get_store();
    fluxdb::ecs::ComponentID comp_id = store->get_id(comp_name);
    if (comp_id == 255) {
        return luaL_error(L, "Component not found: %s", comp_name);
    }

    size_t size = 0;
    const void* data = world->get_entity_component_data(entity, comp_id, size);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }

    // Simplified type dispatching based on size
    if (size == sizeof(int)) {
        // We prioritize integer if it's 4 bytes, common for IDs and gold.
        // For actual positions, we usually use float, but for this demo:
        lua_pushinteger(L, *(const int*)data);
    } else if (size == sizeof(float) && std::string(comp_name) == "pos") {
        lua_pushnumber(L, *(const float*)data);
    } else {
        lua_pushlstring(L, (const char*)data, size);
    }
    return 1;
}

// Lua binding: flux.set(entity, component_name, value)
static int Lua_FluxSet(lua_State* L) {
    auto world = GetWorld(L);
    uint32_t entity = (uint32_t)luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);

    auto store = world->get_store();
    fluxdb::ecs::ComponentID comp_id = store->get_id(comp_name);
    if (comp_id == 255) {
        return luaL_error(L, "Component not found: %s", comp_name);
    }

    size_t size = store->get_info(comp_id).size;
    
    // We handle float and int for the demo
    if (lua_isnumber(L, 3)) {
        if (size == sizeof(int)) {
            // Force integer if size is exactly 4 and not explicitly "pos"
            int val = (int)lua_tointeger(L, 3);
            world->add_component(entity, comp_id, &val);
        } else if (size == sizeof(float)) {
            float val = (float)lua_tonumber(L, 3);
            world->add_component(entity, comp_id, &val);
        }
    } else if (lua_isstring(L, 3)) {
        size_t str_len = 0;
        const char* str = lua_tolstring(L, 3, &str_len);
        world->add_component(entity, comp_id, str);
    }

    return 0;
}

ScriptEngine::ScriptEngine(fluxdb::ecs::World* world) : world_(world) {
    init_lua();
}

ScriptEngine::~ScriptEngine() {
    if (L) lua_close(L);
}

void ScriptEngine::init_lua() {
    L = luaL_newstate();
    luaL_openlibs(L);

    // Store world pointer as a hidden global
    lua_pushlightuserdata(L, world_);
    lua_setglobal(L, "__fluxdb_world");

    register_bindings();
}

void ScriptEngine::register_bindings() {
    lua_newtable(L);

    // Register with world_ as upvalue index 1
    lua_pushlightuserdata(L, world_);
    lua_pushcclosure(L, Lua_FluxGet, 1);
    lua_setfield(L, -2, "get");

    lua_pushlightuserdata(L, world_);
    lua_pushcclosure(L, Lua_FluxSet, 1);
    lua_setfield(L, -2, "set");

    lua_pushlightuserdata(L, world_);
    lua_pushcclosure(L, Lua_FluxSpawn, 1);
    lua_setfield(L, -2, "spawn");

    lua_setglobal(L, "flux");
}

bool ScriptEngine::run_script(const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::cerr << "[ScriptEngine] Lua Error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool ScriptEngine::run_conditional_script(const std::string& lua_code, const std::string& gql_condition) {
    return run_script(lua_code);
}

} // namespace query
} // namespace fluxdb
