#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
    #define FLUX_API __declspec(dllexport)
#else
    #define FLUX_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for a Database instance
typedef struct FluxDB FluxDB;

// Opaque handle for a Query Result
typedef struct FluxResult FluxResult;

// Create a new Flux database instance
FLUX_API FluxDB* flux_init();

// Execute a GQL query string. Returns a result handle or NULL on error.
FLUX_API FluxResult* flux_query(FluxDB* db, const char* sql);

// Get a string representation of the result (e.g. JSON or plan output)
FLUX_API const char* flux_result_get_text(FluxResult* result);

// Free the result object
FLUX_API void flux_free_result(FluxResult* result);

// Close and free the database instance
FLUX_API void flux_close(FluxDB* db);

// Get the last error message
FLUX_API const char* flux_get_last_error();

// --- Networking & State ---

// Manually advance the database tick (History & Subscriptions)
FLUX_API void flux_advance_tick(FluxDB* db);

// Get the current simulation tick
FLUX_API uint64_t flux_get_current_tick(FluxDB* db);

// Generates a binary delta payload since last_ack_tick.
// Returns the actual size written to out_buffer.
FLUX_API size_t flux_get_delta_payload(FluxDB* db, uint64_t last_ack_tick, void* out_buffer, size_t buffer_size);

// --- Scripting (Embedded Lua) ---

// Run a Lua script in the database context.
FLUX_API bool flux_run_script(FluxDB* db, const char* lua_code);

#ifdef __cplusplus
}
#endif
