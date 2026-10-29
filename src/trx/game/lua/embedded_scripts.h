#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *path;
    // Null when the bytecode lies in the blob (g_LUA_EmbeddedBlob) at offset.
    const uint8_t *data;
    size_t offset;
    size_t size;
} LUA_EMBEDDED_SCRIPT;

// Where the bytecode of the scripts that have no data lies, set before the
// Lua runtime starts.
extern const uint8_t *g_LUA_EmbeddedBlob;

// Declares part of the trx.* API, and is required by name.
extern const LUA_EMBEDDED_SCRIPT g_LUA_EmbeddedModules[];

// Declares nothing, and is run for its effect once the API is sealed.
extern const LUA_EMBEDDED_SCRIPT g_LUA_EmbeddedRuntimeScripts[];
