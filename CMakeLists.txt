cmake_minimum_required(VERSION 3.14)

project(FluxDB VERSION 0.2)

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
    core/ecs/versioning.cpp
    core/ecs/query_plans.cpp
    core/ecs/relations.cpp
    core/ecs/delta_set.cpp
    core/ecs/rollback.cpp
    core/ecs/lod.cpp
    core/ecs/fixed.cpp
    core/ecs/scheduler.cpp
    core/ecs/determinism.cpp
    core/ecs/sector_pos.cpp
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

# Temporal Component Versioning Test (#4)
add_executable(test_versioning tests/test_versioning.cpp)
target_link_libraries(test_versioning PRIVATE flux_static)

# Compiled Query Plans Test (#5)
add_executable(test_query_plans tests/test_query_plans.cpp)
target_link_libraries(test_query_plans PRIVATE flux_static)

# Unified Delta Engine Test (#7)
add_executable(test_delta_set tests/test_delta_set.cpp)
target_link_libraries(test_delta_set PRIVATE flux_static)

# Rollback Netcode Test (#8)
add_executable(test_rollback tests/test_rollback.cpp)
target_link_libraries(test_rollback PRIVATE flux_static)

# Native Entity Relationship Graphs Test (#6)
add_executable(test_relations tests/test_relations.cpp)
target_link_libraries(test_relations PRIVATE flux_static)

# Bandwidth-Aware Component LOD Test (#10)
add_executable(test_lod tests/test_lod.cpp)
target_link_libraries(test_lod PRIVATE flux_static)

# Deterministic Lockstep Mode Test (#11)
add_executable(test_lockstep tests/test_lockstep.cpp)
target_link_libraries(test_lockstep PRIVATE flux_static)

add_executable(test_hot_cold tests/test_hot_cold.cpp)
target_link_libraries(test_hot_cold PRIVATE flux_static)

add_executable(test_sector_pos tests/test_sector_pos.cpp)
target_link_libraries(test_sector_pos PRIVATE flux_static)

# Deterministic Seed-Chain Components Test (#21)
add_executable(test_seed_chain tests/test_seed_chain.cpp)
target_link_libraries(test_seed_chain PRIVATE flux_static)

# Deterministic Job Graph Scheduler Test (#32)
add_executable(test_job_graph tests/test_job_graph.cpp)
target_link_libraries(test_job_graph PRIVATE flux_static)

# Time-Travel Collision Queries Test (#13)
add_executable(test_physics tests/test_physics.cpp)
target_link_libraries(test_physics PRIVATE flux_static)

# Priority-Based Chunk Streaming Test (#20)
add_executable(test_streaming tests/test_streaming.cpp)
target_link_libraries(test_streaming PRIVATE flux_static)

# Blackboard & Utility Scoring Test (#14)
add_executable(test_ai_blackboard tests/test_ai_blackboard.cpp)
target_link_libraries(test_ai_blackboard PRIVATE flux_static)

# Behavior Tree / GOAP Test (#16)
add_executable(test_ai_bt_goap tests/test_ai_bt_goap.cpp)
target_link_libraries(test_ai_bt_goap PRIVATE flux_static)

# Spatial Perception Test (#15)
add_executable(test_ai_perception tests/test_ai_perception.cpp)
target_link_libraries(test_ai_perception PRIVATE flux_static)

# Streaming NavMesh Test (#17)
add_executable(test_nav_streaming tests/test_nav_streaming.cpp)
target_link_libraries(test_nav_streaming PRIVATE flux_static)

# Dynamic Nav Graph Test (#18)
add_executable(test_nav_dynamic tests/test_nav_dynamic.cpp)
target_link_libraries(test_nav_dynamic PRIVATE flux_static)

# Time-Travel World Debugger Test (#27)
add_executable(test_debugger tests/test_debugger.cpp)
target_link_libraries(test_debugger PRIVATE flux_static)

# Cache & Fragmentation Profiler Test (#28)
add_executable(test_profiler tests/test_profiler.cpp)
target_link_libraries(test_profiler PRIVATE flux_static)

# Query Plan Explainer Test (#29)
add_executable(test_explainer tests/test_explainer.cpp)
target_link_libraries(test_explainer PRIVATE flux_static)

# Hot-Reload Component Schemas Test (#30)
add_executable(test_hot_reload tests/test_hot_reload.cpp)
target_link_libraries(test_hot_reload PRIVATE flux_static)

# Live Possession Test (#31)
add_executable(test_possession tests/test_possession.cpp)
target_link_libraries(test_possession PRIVATE flux_static)

# Live Schema Evolution Test (#24)
add_executable(test_schema_evolution tests/test_schema_evolution.cpp)
target_link_libraries(test_schema_evolution PRIVATE flux_static)

# AoSoA Layout Test (#2)
add_executable(test_aosoa tests/test_aosoa.cpp)
target_link_libraries(test_aosoa PRIVATE flux_static)

# GPU-Resident Mirror Archetypes Test (#3)
add_executable(test_gpu_mirror tests/test_gpu_mirror.cpp)
target_link_libraries(test_gpu_mirror PRIVATE flux_static)

# GPU Procedural Generation Test (#22)
add_executable(test_procgen tests/test_procgen.cpp)
target_link_libraries(test_procgen PRIVATE flux_static)

# GPU Compute System Compilation Test (#33)
add_executable(test_gpu_compile tests/test_gpu_compile.cpp)
target_link_libraries(test_gpu_compile PRIVATE flux_static)

# Data-Oriented Mod Overlays Test (#25)
add_executable(test_mod_overlay tests/test_mod_overlay.cpp)
target_link_libraries(test_mod_overlay PRIVATE flux_static)

# Sandboxed Mod Query/Script API Test (#26)
add_executable(test_mod_sandbox tests/test_mod_sandbox.cpp)
target_link_libraries(test_mod_sandbox PRIVATE flux_static)

# Causal Divergence Tracing Test (#34)
add_executable(test_causal_divergence tests/test_causal_divergence.cpp)
target_link_libraries(test_causal_divergence PRIVATE flux_static)

# --- Benchmarks (4 secciones x 5) ---
function(fluxdb_bench name)
  add_executable(${name} benchmarks/${name}.cpp)
  target_link_libraries(${name} PRIVATE flux_static)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
endfunction()

# BASICS
fluxdb_bench(bench_basic_1)
fluxdb_bench(bench_basic_2)
fluxdb_bench(bench_basic_3)
fluxdb_bench(bench_basic_4)
fluxdb_bench(bench_basic_5)
# MEDIUMS
fluxdb_bench(bench_medium_1)
fluxdb_bench(bench_medium_2)
fluxdb_bench(bench_medium_3)
fluxdb_bench(bench_medium_4)
fluxdb_bench(bench_medium_5)
# INTERMEDIATES
fluxdb_bench(bench_intermediate_1)
fluxdb_bench(bench_intermediate_2)
fluxdb_bench(bench_intermediate_3)
fluxdb_bench(bench_intermediate_4)
fluxdb_bench(bench_intermediate_5)
# ADVANCED
fluxdb_bench(bench_advanced_1)
fluxdb_bench(bench_advanced_2)
fluxdb_bench(bench_advanced_3)
fluxdb_bench(bench_advanced_4)
fluxdb_bench(bench_advanced_5)

# NIVELES 1 AL 10 & REAL GAME BENCHMARKS
fluxdb_bench(bench_stress_fuzzy)
fluxdb_bench(bench_stress_threads)
fluxdb_bench(bench_large_world)
fluxdb_bench(bench_fuzz_corrupt)
fluxdb_bench(bench_long_running)
fluxdb_bench(bench_cache_analysis)

add_executable(game_asteroids_rts tools/game_asteroids_rts.cpp)
target_link_libraries(game_asteroids_rts PRIVATE flux_static)
target_include_directories(game_asteroids_rts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)

