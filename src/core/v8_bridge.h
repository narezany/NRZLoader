// Hooks into the JavaScript engine embedded in the game.
//
// Retail Android builds export no names for Minecraft's own C++ code, so
// hooking gameplay functions by name is not possible. The scripting engine is
// the exception: its public interface stays exported, which makes it the one
// stable foothold in the process.
//
// This module hooks context creation and script execution, hands the pointers
// to mods, and resolves the engine entry points a mod needs to run its own
// JavaScript.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mcbe/mod_api.h"

namespace mcbe {

class Loader;

namespace v8bridge {

// Entry points resolved from the game. Any of them may be null on a build
// that exports less than expected; check before calling.
struct Resolved {
    void* context_new = nullptr;
    void* script_run = nullptr;
    void* script_compile = nullptr;
    void* function_call = nullptr;
    void* string_new_utf8 = nullptr;
    void* register_extension = nullptr;
    void* isolate_get_current = nullptr;
    void* context_global = nullptr;

    // Most recently observed live objects.
    void* last_isolate = nullptr;
    void* last_context = nullptr;
    uint32_t contexts_seen = 0;
};

const Resolved& resolved();

// Compiles and runs `source` in the most recently seen context.
//
// Must be called on the thread that runs scripts, and only while the engine is
// already executing: the entry points need a live handle scope, which the
// caller of the hooked function has already established. In practice that
// means calling this from a script.context or script.run handler.
McbeResult run_script(const char* source, const char* name);

// Resolves what it can and installs the hooks. Returns the number of hooks
// that went in. Missing symbols are logged, never fatal.
size_t install(Loader& loader, const std::string& config_path);

}  // namespace v8bridge
}  // namespace mcbe
