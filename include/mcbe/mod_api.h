// The contract between the loader and a mod.
//
// Deliberately a C interface: mods are separate shared objects that may be
// built by a different compiler than the loader, and a C++ interface would
// drag name mangling and standard library layout into the ABI.
#ifndef MCBE_MOD_API_H
#define MCBE_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes shape. The loader refuses mods built
// against a different value.
#define MCBE_ABI_VERSION 3u

// The loader's own version, which a packaged mod can require a range of
// through "minLoader" and "maxLoader" in its mod.json. Kept in step with the
// VERSION file at the root of the project and with the launcher's copy; a
// test fails when they drift apart.
#define MCBE_LOADER_VERSION "1.9.1"

typedef enum McbeLogLevel {
    MCBE_LOG_DEBUG = 0,
    MCBE_LOG_INFO = 1,
    MCBE_LOG_WARN = 2,
    MCBE_LOG_ERROR = 3,
} McbeLogLevel;

typedef enum McbeResult {
    MCBE_OK = 0,
    MCBE_ERR_NOT_FOUND = 1,
    MCBE_ERR_HOOK_FAILED = 2,
    MCBE_ERR_BAD_ARGUMENT = 3,
    MCBE_ERR_ABI_MISMATCH = 4,
} McbeResult;

typedef struct McbeModInfo {
    uint32_t abi_version;   // must equal MCBE_ABI_VERSION
    const char* id;         // unique, e.g. "example.hello"
    const char* name;
    const char* version;
    const char* target_game;  // game build this mod was written against
} McbeModInfo;

typedef struct McbeEvent {
    const char* name;
    void* data;      // event specific payload, see events.h
    int cancelled;   // set to non-zero to suppress the default behaviour
} McbeEvent;

typedef void (*McbeEventHandler)(McbeEvent* event, void* user_data);

// Raw entry points into the JavaScript engine embedded in the game, plus the
// most recent live objects seen by the loader.
//
// These are handed over as plain pointers because the engine's headers are not
// part of this ABI. A mod that uses them is responsible for calling them with
// the right signatures. Any field may be null on a build that exports less
// than expected, so check before calling.
typedef struct McbeV8 {
    void* context_new;
    void* script_run;
    void* script_compile;
    void* function_call;
    void* string_new_utf8;
    void* register_extension;
    void* isolate_get_current;
    void* context_global;

    void* last_isolate;
    void* last_context;
    uint32_t contexts_seen;
} McbeV8;

// Everything a mod is allowed to do, handed over at init time.
typedef struct McbeApi {
    uint32_t abi_version;

    // Address lookup inside the game library.
    void* (*resolve_symbol)(const char* mangled_name);
    void* (*scan_signature)(const char* pattern);

    // Detouring. `out_original` receives a callable pointer to the untouched
    // function; call it to keep the game's behaviour.
    McbeResult (*install_hook)(void* target, void* detour, void** out_original);
    McbeResult (*remove_hook)(void* target);
    McbeResult (*hook_vtable)(void* object, size_t index, void* detour, void** out_original);

    // Events raised by the loader's own bindings.
    McbeResult (*subscribe)(const char* event_name, McbeEventHandler handler, void* user_data);

    void (*log)(McbeLogLevel level, const char* message);

    const char* (*game_version)(void);  // as reported by the loaded library
    const char* (*data_directory)(void);  // writable directory for mod files

    // Never null; the struct it points at may still hold null entries.
    const McbeV8* (*v8)(void);

    // Compiles and runs JavaScript inside the game's script engine.
    // Only valid from a script.context or script.run handler: outside those
    // the engine has no live handle scope and the call will fail or crash.
    McbeResult (*run_script)(const char* source, const char* name);
} McbeApi;

// ---------------------------------------------------------------------------
// Symbols every mod must export
// ---------------------------------------------------------------------------

// Describes the mod. Called before init; must not touch the game.
const McbeModInfo* mcbe_mod_info(void);

// Called once, after the game library is mapped and symbols are resolvable.
// Return MCBE_OK to stay loaded; anything else unloads the mod.
McbeResult mcbe_mod_init(const McbeApi* api);

// Optional. Called before the mod is unloaded.
void mcbe_mod_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MCBE_MOD_API_H
