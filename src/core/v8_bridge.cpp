#include "v8_bridge.h"

#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>

#include "js_natives.h"
#include "loader.h"
#include "mod_manifest.h"
#include "paths.h"
#include "log.h"
#include "mcbe/events.h"

namespace mcbe::v8bridge {
namespace {

Resolved g_resolved;

void* g_original_context_new = nullptr;
void* g_original_script_run = nullptr;

// Confirmed against a retail 1.26 arm64 build. These are exact strings read
// out of the library's dynamic symbol table, not guesses.
struct Entry {
    const char* key;
    const char* symbol;
    void* Resolved::*field;
};

const Entry kEntries[] = {
    {"context.new",
     "_ZN2v87Context3NewEPNS_7IsolateEPNS_22ExtensionConfigurationENS_10MaybeLocalINS_"
     "14ObjectTemplateEEENS5_INS_5ValueEEENS_33DeserializeInternalFieldsCallbackEPNS_"
     "14MicrotaskQueueE",
     &Resolved::context_new},
    {"script.run", "_ZN2v86Script3RunENS_5LocalINS_7ContextEEE", &Resolved::script_run},
    {"script.compile",
     "_ZN2v86Script7CompileENS_5LocalINS_7ContextEEENS1_INS_6StringEEEPNS_12ScriptOriginE",
     &Resolved::script_compile},
    {"function.call",
     "_ZN2v88Function4CallENS_5LocalINS_7ContextEEENS1_INS_5ValueEEEiPS5_",
     &Resolved::function_call},
    {"string.new",
     "_ZN2v86String11NewFromUtf8EPNS_7IsolateEPKcNS_13NewStringTypeEi",
     &Resolved::string_new_utf8},
    {"register.extension",
     "_ZN2v817RegisterExtensionENSt6__ndk110unique_ptrINS_9ExtensionENS0_14default_deleteIS2_EEEE",
     &Resolved::register_extension},
    {"isolate.current", "_ZN2v87Isolate10GetCurrentEv", &Resolved::isolate_get_current},
    {"context.global", "_ZN2v87Context6GlobalEv", &Resolved::context_global},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

void maybe_run_javascript(void* context, bool is_game_script, uint32_t sequence);

// Every argument here is integer or pointer sized and there are no more than
// eight, so forwarding them unchanged is correct on AArch64 regardless of how
// the real declaration groups them into structs.
void* detour_context_new(void* isolate, void* extension_configuration, void* global_template,
                         void* global_object, void* deserialize_a, void* deserialize_b,
                         void* microtask_queue) {
    auto original = reinterpret_cast<void* (*)(void*, void*, void*, void*, void*, void*, void*)>(
        g_original_context_new);

    void* context = nullptr;
    if (original != nullptr) {
        context = original(isolate, extension_configuration, global_template, global_object,
                           deserialize_a, deserialize_b, microtask_queue);
    }

    g_resolved.last_isolate = isolate;
    g_resolved.last_context = context;
    g_resolved.contexts_seen += 1;

    McbeScriptContextEvent payload;
    payload.isolate = isolate;
    payload.context = context;
    // The user interface embeds the same engine, and it builds its contexts
    // first. Treating anything past the first batch as game script is a rough
    // heuristic; a mod that needs certainty should inspect the globals.
    payload.is_game_script = g_resolved.contexts_seen > 1 ? 1 : 0;
    payload.sequence = g_resolved.contexts_seen;

    MCBE_LOGI("v8 context #%u isolate=%p context=%p", payload.sequence, isolate, context);
    Loader::instance().events().dispatch(MCBE_EVENT_SCRIPT_CONTEXT, &payload);

    // Native functions and the wrapper around them go in before any mod
    // script runs, so a mod can call them from its top level.
    if (js::install(isolate, context)) {
        const McbeResult shim = run_script(js::shim_source(), "nrz shim");
        if (shim != MCBE_OK) MCBE_LOGW("the script wrapper did not install");
    }

    // Running here rather than waiting for the game to execute a script: the
    // interface contexts never call the public run entry point, so waiting can
    // mean waiting forever.
    maybe_run_javascript(context, payload.is_game_script != 0, payload.sequence);

    return context;
}

bool g_inside_our_script = false;
std::set<void*> g_served_contexts;

// Which contexts scripts get run in. The user interface embeds the same
// engine as the game's scripting, and on a retail build the interface builds
// its contexts first, so "all" is the default: it costs a failed compile at
// worst and it is the only setting that works before you know which context
// is which.
enum class RunPolicy { All, GameOnly, FirstOnly, Never };
RunPolicy g_policy = RunPolicy::All;

// Runs every .js file in the mods directory, once per context.
void run_javascript_in_current_context() {
    if (g_inside_our_script) return;

    const std::string directory = Loader::instance().mods_directory();

    // Both loose scripts and those inside a packaged mod's directory.
    const std::vector<std::string> files = paths::collect_mod_files(directory, ".js");

    if (files.empty()) {
        MCBE_LOGI("no .js files in %s", directory.c_str());
        return;
    }

    const std::string config = Loader::instance().data_directory() + "/config";

    g_inside_our_script = true;
    for (const std::string& path : files) {
        std::string reason;
        if (!mods::should_run(path, directory, config, MCBE_LOADER_VERSION, reason)) {
            MCBE_LOGI("skipping %s: %s", path.c_str(), reason.c_str());
            continue;
        }

        std::ifstream file(path);
        if (!file) continue;
        const std::string source((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
        const McbeResult result = run_script(source.c_str(), path.c_str());
        MCBE_LOGI("javascript %s: %s", path.c_str(), result == MCBE_OK ? "ok" : "failed");
    }
    g_inside_our_script = false;
}

// Decides whether this context should get the mods, and remembers the ones
// already served so a context is never run twice.
void maybe_run_javascript(void* context, bool is_game_script, uint32_t sequence) {
    if (g_policy == RunPolicy::Never || context == nullptr) return;
    if (g_policy == RunPolicy::GameOnly && !is_game_script) return;
    if (g_policy == RunPolicy::FirstOnly && sequence != 1) return;
    if (!g_served_contexts.insert(context).second) return;

    MCBE_LOGI("running javascript in context #%u", sequence);
    run_javascript_in_current_context();
}

void* detour_script_run(void* script, void* context) {
    McbeScriptRunEvent payload;
    payload.script = script;
    payload.context = context;
    Loader::instance().events().dispatch(MCBE_EVENT_SCRIPT_RUN, &payload);

    auto original = reinterpret_cast<void* (*)(void*, void*)>(g_original_script_run);
    void* result = original == nullptr ? nullptr : original(script, context);

    g_resolved.last_context = context;
    maybe_run_javascript(context, true, g_resolved.contexts_seen);
    return result;
}

// Lets a build with different mangling be corrected without a rebuild.
std::unordered_map<std::string, std::string> read_overrides(const std::string& path) {
    std::unordered_map<std::string, std::string> overrides;
    std::ifstream file(path);
    if (!file) return overrides;

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        auto trim = [](std::string text) {
            const size_t begin = text.find_first_not_of(" \t\r\n");
            const size_t end = text.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        };

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (value.empty()) continue;
        if (key.rfind("v8.", 0) == 0) overrides[key.substr(3)] = value;
        if (key.rfind("script.", 0) == 0) overrides[key.substr(7)] = value;
    }
    return overrides;
}

}  // namespace

const Resolved& resolved() { return g_resolved; }

McbeResult run_script(const char* source, const char* name) {
    if (source == nullptr) return MCBE_ERR_BAD_ARGUMENT;
    if (g_resolved.string_new_utf8 == nullptr || g_resolved.script_compile == nullptr ||
        g_original_script_run == nullptr) {
        return MCBE_ERR_NOT_FOUND;
    }

    void* isolate = g_resolved.last_isolate;
    void* context = g_resolved.last_context;
    if (isolate == nullptr || context == nullptr) return MCBE_ERR_NOT_FOUND;

    // v8::NewStringType::kNormal is 0; -1 means "measure the string yourself".
    auto new_string =
        reinterpret_cast<void* (*)(void*, const char*, int, int)>(g_resolved.string_new_utf8);
    void* text = new_string(isolate, source, 0, -1);
    if (text == nullptr) {
        MCBE_LOGE("%s: could not build a string", name);
        return MCBE_ERR_HOOK_FAILED;
    }

    // An empty MaybeLocal and a null Local are both a null pointer here, which
    // is what makes it safe to pass one straight into the next call.
    auto compile = reinterpret_cast<void* (*)(void*, void*, void*)>(g_resolved.script_compile);
    void* script = compile(context, text, nullptr);
    if (script == nullptr) {
        MCBE_LOGE("%s: did not compile", name);
        return MCBE_ERR_HOOK_FAILED;
    }

    // Deliberately the trampoline rather than the hooked address, so running
    // our own script does not re-enter the detour.
    auto run = reinterpret_cast<void* (*)(void*, void*)>(g_original_script_run);
    void* value = run(script, context);
    if (value == nullptr) {
        MCBE_LOGE("%s: threw during execution", name);
        return MCBE_ERR_HOOK_FAILED;
    }
    return MCBE_OK;
}

size_t install(Loader& loader, const std::string& config_path) {
    const auto overrides = read_overrides(config_path);

    const auto policy_entry = overrides.find("run_in");
    if (policy_entry != overrides.end()) {
        const std::string& value = policy_entry->second;
        if (value == "all") g_policy = RunPolicy::All;
        else if (value == "game") g_policy = RunPolicy::GameOnly;
        else if (value == "first") g_policy = RunPolicy::FirstOnly;
        else if (value == "none") g_policy = RunPolicy::Never;
        else MCBE_LOGW("unknown script.run_in value: %s", value.c_str());
    }

    size_t found = 0;
    for (size_t index = 0; index < kEntryCount; ++index) {
        const Entry& entry = kEntries[index];
        std::string symbol = entry.symbol;
        const auto override_entry = overrides.find(entry.key);
        if (override_entry != overrides.end()) symbol = override_entry->second;

        void* address = loader.symbols().resolve(symbol);
        g_resolved.*(entry.field) = address;

        if (address == nullptr) {
            MCBE_LOGW("v8 %-18s missing", entry.key);
        } else {
            MCBE_LOGI("v8 %-18s -> %p", entry.key, address);
            ++found;
        }
    }

    if (found == 0) {
        MCBE_LOGE("no v8 entry points resolved; this build may not embed the engine");
        return 0;
    }

    size_t hooks = 0;

    if (g_resolved.context_new != nullptr) {
        if (loader.install_hook(g_resolved.context_new,
                                reinterpret_cast<void*>(&detour_context_new),
                                &g_original_context_new) == MCBE_OK) {
            ++hooks;
        } else {
            MCBE_LOGE("failed to hook v8::Context::New");
        }
    }

    if (g_resolved.script_run != nullptr) {
        if (loader.install_hook(g_resolved.script_run, reinterpret_cast<void*>(&detour_script_run),
                                &g_original_script_run) == MCBE_OK) {
            ++hooks;
        } else {
            MCBE_LOGE("failed to hook v8::Script::Run");
        }
    }

    const size_t natives = js::resolve(loader);
    MCBE_LOGI("js natives: %zu entry points, %s", natives,
              js::ready() ? "ready" : "incomplete, native calls disabled");

    MCBE_LOGI("v8 bridge: %zu of %zu entry points, %zu hooks", found, kEntryCount, hooks);
    return hooks;
}

}  // namespace mcbe::v8bridge
