cmake_minimum_required(VERSION 3.14)

project(FluxDB VERSION 0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)
FetchContent_Declare(
  lua_src
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.6
)
FetchContent_MakeAvailable(lua_src)

# Define Lua library manually since the official repo doesn't have a CMakeLists.txt
set(LUA_SOURCES
    ${lua_src_SOURCE_DIR}/lapi.c ${lua_src_SOURCE_DIR}/lcode.c ${lua_src_SOURCE_DIR}/lctype.c ${lua_src_SOURCE_DIR}/ldebug.c
    ${lua_src_SOURCE_DIR}/ldo.c ${lua_src_SOURCE_DIR}/ldump.c ${lua_src_SOURCE_DIR}/lfunc.c ${lua_src_SOURCE_DIR}/lgc.c
    ${lua_src_SOURCE_DIR}/llex.c ${lua_src_SOURCE_DIR}/lmem.c ${lua_src_SOURCE_DIR}/lobject.c ${lua_src_SOURCE_DIR}/lopcodes.c
    ${lua_src_SOURCE_DIR}/lparser.c ${lua_src_SOURCE_DIR}/lstate.c ${lua_src_SOURCE_DIR}/lstring.c ${lua_src_SOURCE_DIR}/ltable.c
    ${lua_src_SOURCE_DIR}/ltm.c ${lua_src_SOURCE_DIR}/lundump.c ${lua_src_SOURCE_DIR}/lvm.c ${lua_src_SOURCE_DIR}/lzio.c
    ${lua_src_SOURCE_DIR}/lauxlib.c ${lua_src_SOURCE_DIR}/lbaselib.c ${lua_src_SOURCE_DIR}/lcorolib.c ${lua_src_SOURCE_DIR}/ldblib.c
    ${lua_src_SOURCE_DIR}/liolib.c ${lua_src_SOURCE_DIR}/lmathlib.c ${lua_src_SOURCE_DIR}/loslib.c ${lua_src_SOURCE_DIR}/lstrlib.c
    ${lua_src_SOURCE_DIR}/ltablib.c ${lua_src_SOURCE_DIR}/lutf8lib.c ${lua_src_SOURCE_DIR}/loadlib.c ${lua_src_SOURCE_DIR}/linit.c
)
add_library(lua_lib STATIC ${LUA_SOURCES})
target_include_directories(lua_lib PUBLIC ${lua_src_SOURCE_DIR})



# Core Sources
set(CORE_SOURCES
    core/query/lexer.cpp
    core/query/parser.cpp
    core/query/planner.cpp
    core/query/executor.cpp
    core/query/flux_c_api.cpp
    core/ecs/component_store.cpp
    core/ecs/archetype.cpp
    core/ecs/world.cpp
    core/ecs/history.cpp
    core/query/pubsub.cpp
    core/query/scripting.cpp
    core/network/delta_compression.cpp
    core/spatial/octree.cpp
    core/storage/page_manager.cpp
    core/storage/buffer_pool.cpp
    core/storage/wal.cpp
    core/storage/vacuum.cpp
)

# Shared Library (C-API Bridge for bindings)
add_library(flux SHARED ${CORE_SOURCES})
target_include_directories(flux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/headers)
target_link_libraries(flux PRIVATE lua_lib)

# Static Library (For internal C++ tests and tools)
add_library(flux_static STATIC ${CORE_SOURCES})
target_include_directories(flux_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/headers)
target_link_libraries(flux_static PUBLIC lua_lib)

# --- Tests & Executables ---

# Main Engine Entry (CLI testing equivalent)
add_executable(flux_cli main.cpp)
target_link_libraries(flux_cli PRIVATE flux_static)

# ECS Performance Test
add_executable(test_ecs tests/test_ecs.cpp)
target_link_libraries(test_ecs PRIVATE flux_static)

# Storage Engine Test
add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE flux_static)

# WAL Crash Recovery Test
add_executable(test_wal_recovery tests/test_wal_recovery.cpp)
target_link_libraries(test_wal_recovery PRIVATE flux_static)

# Concurrency Thread-Safety Test
add_executable(test_concurrency tests/test_concurrency.cpp)
target_link_libraries(test_concurrency PRIVATE flux_static)

# Time-Travel Deterministic Replay Test
add_executable(test_time_travel tests/test_time_travel.cpp)
target_link_libraries(test_time_travel PRIVATE flux_static)

# Reactive Spatial Pub/Sub Test
add_executable(test_pubsub tests/test_pubsub.cpp)
target_link_libraries(test_pubsub PRIVATE flux_static)

# Network Delta Compression Test
add_executable(test_delta_compression tests/test_delta_compression.cpp)
target_link_libraries(test_delta_compression PRIVATE flux_static)

# Embedded Lua Scripting Test
add_executable(test_scripting tests/test_scripting.cpp)
target_link_libraries(test_scripting PRIVATE flux_static)

add_executable(test_spatial_index tests/test_spatial_index.cpp)
target_link_libraries(test_spatial_index PRIVATE flux_static)
