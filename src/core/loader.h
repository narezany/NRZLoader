// Loader singleton: owns symbol resolution, hooks, events and mods.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "event_bus.h"
#include "hook/inline_hook.h"
#include "mcbe/mod_api.h"
#include "symbols.h"

namespace mcbe {

struct LoadedMod {
    void* handle = nullptr;
    const McbeModInfo* info = nullptr;
    void (*shutdown)() = nullptr;
    std::string path;
};

class Loader {
public:
    static Loader& instance();

    // Full startup: locate the game, read config, hook bindings, load mods.
    void start();
    void stop();

    SymbolResolver& symbols() { return symbols_; }
    EventBus& events() { return events_; }
    const McbeApi* api() const { return &api_; }

    const std::string& game_version() const { return game_version_; }
    const std::string& data_directory() const { return data_directory_; }
    const std::string& mods_directory() const { return mods_directory_; }

    // Used by the API shims and by the bindings.
    McbeResult install_hook(void* target, void* detour, void** out_original);
    McbeResult remove_hook(void* target);

private:
    Loader();

    void detect_data_directory();
    void load_configuration();
    void report_capabilities();
    void load_mods();
    void unload_mods();

    SymbolResolver symbols_;
    EventBus events_;
    McbeApi api_{};

    std::mutex hooks_mutex_;
    std::unordered_map<void*, std::unique_ptr<hook::InlineHook>> hooks_;

    std::vector<LoadedMod> mods_;

    std::string game_library_ = "libminecraftpe.so";
    std::string game_version_ = "unknown";
    std::string data_directory_;
    std::string mods_directory_;
    std::string bindings_file_;
    bool started_ = false;
    // True when the loader had to fall back off /sdcard, which means the app
    // is missing all-files access.
    bool storage_limited_ = false;
};

}  // namespace mcbe
