#include "loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>

#include "bindings.h"
#include "jni_bridge.h"
#include "mod_manifest.h"
#include "screen_fx.h"
#include "ui_bridge.h"
#include "vtable_probe.h"
#include "v8_bridge.h"
#include "log.h"
#include "paths.h"
#include "mcbe/events.h"

namespace mcbe {
namespace {

Loader* g_loader = nullptr;

// --- McbeApi shims ---------------------------------------------------------

void* api_resolve_symbol(const char* mangled_name) {
    if (mangled_name == nullptr) return nullptr;
    return g_loader->symbols().resolve(mangled_name);
}

void* api_scan_signature(const char* pattern) {
    if (pattern == nullptr) return nullptr;
    return g_loader->symbols().scan_signature(pattern);
}

McbeResult api_install_hook(void* target, void* detour, void** out_original) {
    return g_loader->install_hook(target, detour, out_original);
}

McbeResult api_remove_hook(void* target) { return g_loader->remove_hook(target); }

McbeResult api_hook_vtable(void* object, size_t index, void* detour, void** out_original) {
    const auto status = hook::hook_vtable(object, index, detour, out_original);
    return status == hook::HookStatus::Ok ? MCBE_OK : MCBE_ERR_HOOK_FAILED;
}

McbeResult api_subscribe(const char* event_name, McbeEventHandler handler, void* user_data) {
    if (event_name == nullptr || handler == nullptr) return MCBE_ERR_BAD_ARGUMENT;
    g_loader->events().subscribe(event_name, handler, user_data);
    return MCBE_OK;
}

void api_log(McbeLogLevel level, const char* message) {
    if (message == nullptr) return;
    log::write(static_cast<log::Level>(level), "NRZMod", "%s", message);
}

const char* api_game_version() { return g_loader->game_version().c_str(); }
const char* api_data_directory() { return g_loader->data_directory().c_str(); }

McbeResult api_run_script(const char* source, const char* name) {
    return v8bridge::run_script(source, name == nullptr ? "mod script" : name);
}

const McbeV8* api_v8() {
    // The bridge's layout mirrors McbeV8 field for field, so the public view
    // is just a reinterpretation of it rather than a copy that could go stale.
    static_assert(sizeof(v8bridge::Resolved) == sizeof(McbeV8),
                  "McbeV8 and v8bridge::Resolved must stay in sync");
    return reinterpret_cast<const McbeV8*>(&v8bridge::resolved());
}

// Reads a single `key = value` line out of the config file.
std::string read_setting(const std::string& path, const std::string& key,
                         const std::string& fallback) {
    std::ifstream file(path);
    if (!file) return fallback;

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
        if (trim(line.substr(0, equals)) == key) {
            const std::string value = trim(line.substr(equals + 1));
            if (!value.empty()) return value;
        }
    }
    return fallback;
}

// Written out on first run so the options are discoverable without reading
// the source. Only the values that differ from the built-in defaults matter.
const char* kDefaultConfig = R"(# NRZLoader settings
#
# Everything here is optional: the loader has working defaults compiled in.
# Uncomment a line to change it. Restart the game to apply.

# Where to run javascript mods.
#   all   every javascript context, including the user interface (default)
#   game  only contexts that look like the game's scripting engine
#   first only the first context
#   none  never
#script.run_in = all

# Whether to ask for all-files access on startup.
#   auto   only when the loader could not write to /sdcard (default)
#   always ask on every launch until granted
#   never  never ask
#storage.request = auto

# Symbol names, if a game update moves them. The log lists what resolved.
#v8.context.new = _ZN2v87Context3NewE...
#jni.activity_init = Java_com_google_androidgamesdk_GameActivity_initializeNativeCode

# Effects drawn over the finished picture, as name=strength from 0 to 1.
# Several can run at once: screen.effects = crt=0.5,chroma=0.3
# Names: pixelate, fisheye, wave, glitch, chroma, crt, vignette, grayscale,
# invert. A mod can change these while the game runs through nrz.fx.set().
#screen.effects = crt=0.6

# Разведка виртуальных методов. Имён у них нет — их вырезали и из клиента, и
# из сервера, — но по тому, когда метод зовут, видно, что он делает. Укажите
# класс, поиграйте полминуты и прочтите reports/slots.txt: двадцать вызовов в
# секунду это тик, один при ударе — обработка урона.
# Адреса таблиц лаунчер кладёт в config/vtables.conf сам.
#probe.class = Actor

# Окошко поверх игры с тем, что загрузчик про неё знает: прыгает ли игрок,
# плывёт ли, ломает ли блок, сколько чего сделал. Работает, когда включена
# разведка: числа берутся из её счётчиков.
#hud = off
#
# Номера методов для окошка. У каждой сборки игры они свои — эти найдены
# разведкой на 1.26.23.1. Как узнать свои, написано в reports/slots.txt.
#hud.break = 111
#hud.place = 103,112
#hud.hit = 144
#hud.eat = 68,69
#hud.jump = 62,63
#hud.swim = 37
#hud.use = 149
#hud.look = 114,187
#hud.frame = 35
#hud.tick = 25
)";

void write_default_config(const std::string& path) {
    std::ifstream existing(path);
    if (existing.good()) return;

    std::ofstream file(path);
    if (!file) {
        MCBE_LOGW("could not create %s", path.c_str());
        return;
    }
    file << kDefaultConfig;
    MCBE_LOGI("wrote a default config to %s", path.c_str());
}

bool directory_exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

}  // namespace

Loader::Loader() {
    api_.abi_version = MCBE_ABI_VERSION;
    api_.resolve_symbol = api_resolve_symbol;
    api_.scan_signature = api_scan_signature;
    api_.install_hook = api_install_hook;
    api_.remove_hook = api_remove_hook;
    api_.hook_vtable = api_hook_vtable;
    api_.subscribe = api_subscribe;
    api_.log = api_log;
    api_.game_version = api_game_version;
    api_.v8 = api_v8;
    api_.run_script = api_run_script;
    api_.data_directory = api_data_directory;
}

Loader& Loader::instance() {
    static Loader loader;
    g_loader = &loader;
    return loader;
}

void Loader::detect_data_directory() {
    const paths::Layout layout = paths::choose(paths::default_candidates());

    if (!layout.valid) {
        MCBE_LOGE("no writable directory found; grant the app all-files access");
        data_directory_ = "/sdcard/NRZLoader";
        mods_directory_ = data_directory_ + "/mods";
        bindings_file_ = data_directory_ + "/config/bindings.conf";
        return;
    }

    // From here on the log is also on disk, readable with any file manager.
    log::set_file(layout.log_file.c_str());

    storage_limited_ = layout.root.find("/NRZLoader") == std::string::npos ||
                       layout.root.find("Android/data") != std::string::npos ||
                       layout.root.find("/data/data/") != std::string::npos;

    data_directory_ = layout.root;
    mods_directory_ = layout.mods;
    bindings_file_ = layout.config + "/bindings.conf";

    MCBE_LOGI("loader directory: %s", data_directory_.c_str());
    MCBE_LOGI("  mods:   %s", mods_directory_.c_str());
    MCBE_LOGI("  config: %s", bindings_file_.c_str());
    MCBE_LOGI("  log:    %s", layout.log_file.c_str());

    write_default_config(bindings_file_);
}

void Loader::report_capabilities() {
    // Written to the log so the next round of work can be planned from what
    // this build actually contains, rather than from guesses.
    struct Probe {
        const char* label;
        const char* fragment;
    };
    const Probe probes[] = {
        {"v8 Object::Get", "_ZN2v86Object3GetENS_5LocalINS_7ContextEEENS1_INS_5ValueEEE"},
        {"v8 String::WriteUtf8", "6String9WriteUtf8E"},
        {"v8 Value::ToString", "5Value8ToStringE"},
        {"v8 ObjectTemplate::New", "14ObjectTemplate3NewE"},
        {"cohtml Document::createElement", "8Document13CreateElement"},
        {"cohtml Node::appendChild", "4Node11AppendChild"},
        {"cohtml View::LoadURL", "4View7LoadURL"},
        {"cohtml ViewImpl::LoadURL", "8ViewImpl7LoadURL"},
        {"cohtml Scripting::ExecuteScript", "9Scripting13ExecuteScriptE"},
        {"cohtml ViewImpl::RegisterForEvent", "8ViewImpl16RegisterForEventE"},
        {"cohtml ViewBinder::TriggerEventNative", "10ViewBinder18TriggerEventNativeE"},
        {"cohtml Document::body", "8Document4Body"},
        {"cohtml InternedString", "14InternedStringC1E"},
    };

    MCBE_LOGI("--- capability report ---");
    for (const Probe& probe : probes) {
        const std::vector<std::string> matches = symbols_.find_containing(probe.fragment);
        if (matches.empty()) {
            MCBE_LOGI("  %-38s absent", probe.label);
        } else {
            MCBE_LOGI("  %-38s %zu, e.g. %.90s", probe.label, matches.size(),
                      matches.front().c_str());
        }
    }
    MCBE_LOGI("--- end of report ---");
}

void Loader::load_configuration() {
    const std::string offsets = data_directory_ + "/config/symbols.map";
    if (symbols_.load_offset_map(offsets)) {
        MCBE_LOGI("loaded offset map from %s", offsets.c_str());
    }

    // The game build string is itself just a symbol in the library.
    if (void* version = symbols_.resolve("_ZN14SharedConstants15CurrentVersionEv")) {
        (void)version;  // calling it needs the game's std::string ABI; skipped
    }
}

McbeResult Loader::install_hook(void* target, void* detour, void** out_original) {
    if (target == nullptr || detour == nullptr) return MCBE_ERR_BAD_ARGUMENT;

    std::lock_guard<std::mutex> guard(hooks_mutex_);
    if (hooks_.count(target) != 0) return MCBE_ERR_HOOK_FAILED;

    auto hook = std::make_unique<hook::InlineHook>();
    const auto status = hook->install(target, detour);
    if (status != hook::HookStatus::Ok) {
        MCBE_LOGE("hook at %p failed: %s", target, hook::status_string(status));
        return MCBE_ERR_HOOK_FAILED;
    }
    if (out_original != nullptr) *out_original = hook->original_raw();
    hooks_.emplace(target, std::move(hook));
    return MCBE_OK;
}

McbeResult Loader::remove_hook(void* target) {
    std::lock_guard<std::mutex> guard(hooks_mutex_);
    const auto entry = hooks_.find(target);
    if (entry == hooks_.end()) return MCBE_ERR_NOT_FOUND;
    entry->second->remove();
    hooks_.erase(entry);
    return MCBE_OK;
}

void Loader::load_mods() {
    if (!directory_exists(mods_directory_)) {
        MCBE_LOGW("no mods directory at %s", mods_directory_.c_str());
        return;
    }

    // Stable order so a broken mod always fails in the same place.
    const std::vector<std::string> files = paths::collect_mod_files(mods_directory_, ".so");

    for (const std::string& path : files) {
        std::string reason;
        if (!mods::should_run(path, mods_directory_, data_directory_ + "/config",
                              MCBE_LOADER_VERSION, reason)) {
            MCBE_LOGI("skipping %s: %s", path.c_str(), reason.c_str());
            continue;
        }

        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            MCBE_LOGE("cannot load %s: %s", path.c_str(), dlerror());
            continue;
        }

        auto info_fn = reinterpret_cast<const McbeModInfo* (*)()>(dlsym(handle, "mcbe_mod_info"));
        auto init_fn = reinterpret_cast<McbeResult (*)(const McbeApi*)>(dlsym(handle, "mcbe_mod_init"));
        if (info_fn == nullptr || init_fn == nullptr) {
            MCBE_LOGE("%s does not export the mod entry points", path.c_str());
            dlclose(handle);
            continue;
        }

        const McbeModInfo* info = info_fn();
        if (info == nullptr || info->abi_version != MCBE_ABI_VERSION) {
            MCBE_LOGE("%s targets ABI %u, loader provides %u", path.c_str(),
                      info == nullptr ? 0u : info->abi_version, MCBE_ABI_VERSION);
            dlclose(handle);
            continue;
        }

        if (info->target_game != nullptr && game_version_ != "unknown" &&
            game_version_ != info->target_game) {
            MCBE_LOGW("%s targets game %s but this is %s; loading anyway", info->id,
                      info->target_game, game_version_.c_str());
        }

        const McbeResult result = init_fn(&api_);
        if (result != MCBE_OK) {
            MCBE_LOGE("%s refused to initialise (%d)", info->id, static_cast<int>(result));
            dlclose(handle);
            continue;
        }

        LoadedMod mod;
        mod.handle = handle;
        mod.info = info;
        mod.shutdown = reinterpret_cast<void (*)()>(dlsym(handle, "mcbe_mod_shutdown"));
        mod.path = path;
        mods_.push_back(mod);

        MCBE_LOGI("loaded mod %s %s (%s)", info->id, info->version, info->name);
    }

    MCBE_LOGI("%zu mods active", mods_.size());
}

void Loader::unload_mods() {
    for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
        if (it->shutdown != nullptr) it->shutdown();
        events_.unsubscribe_owner(it->handle);
        dlclose(it->handle);
    }
    mods_.clear();
}

void Loader::start() {
    if (started_) return;
    started_ = true;

    MCBE_LOGI("NRZLoader %s starting", MCBE_LOADER_VERSION);
    detect_data_directory();

    if (!symbols_.initialise(game_library_)) {
        MCBE_LOGE("%s is not mapped yet; loader is idle", game_library_.c_str());
        return;
    }

    // Hooked before anything else that can take time: the game calls its JNI
    // entry point early, and missing it means no way to ask for storage.
    jni::install(*this, bindings_file_);

    const std::string storage_mode = read_setting(bindings_file_, "storage.request", "auto");
    if (storage_mode == "always") {
        MCBE_LOGI("storage.request=always; asking for all-files access regardless");
        jni::request_all_files_access();
    } else if (storage_mode == "never") {
        MCBE_LOGI("storage.request=never; not asking for all-files access");
    } else {
        // Asked for on the basis of whether the permission is actually held,
        // not on whether a directory left over from an earlier install happens
        // to be writable. The request itself checks and stays quiet when the
        // access is already there.
        jni::request_all_files_access();
    }

    load_configuration();

    const size_t bound = bindings::install_all(*this, bindings_file_);
    MCBE_LOGI("%zu of %zu native bindings active", bound, bindings::binding_count());

    // Retail builds strip the game's own names, which leaves the script
    // engine as the only thing worth hooking by name.
    const size_t script_hooks = v8bridge::install(*this, bindings_file_);
    if (bound == 0 && script_hooks == 0) {
        MCBE_LOGE("nothing could be hooked; check bindings.conf against your build");
    }

    report_capabilities();

    const size_t ui_hooks = ui::install(*this);
    MCBE_LOGI("%zu interface hooks active", ui_hooks);

    // Effects go on last of the hooks: they touch the graphics context, and
    // nothing else the loader does should be waiting behind that.
    if (fx::install(*this)) {
        const std::string effects = read_setting(bindings_file_, "screen.effects", "");
        if (!effects.empty()) fx::set(effects);
    }

    // Счётчики на таблицу — последними: они подменяют указатели в игре, и
    // включаются только когда об этом попросили явно.
    probe::install(*this, bindings_file_);

    load_mods();
    events_.dispatch(MCBE_EVENT_READY, nullptr);
}

void Loader::stop() {
    if (!started_) return;
    unload_mods();

    std::lock_guard<std::mutex> guard(hooks_mutex_);
    for (auto& [target, hook] : hooks_) hook->remove();
    hooks_.clear();
    started_ = false;
}

}  // namespace mcbe
