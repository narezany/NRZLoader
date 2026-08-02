#include "paths.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

namespace mcbe::paths {

bool ensure_directory(const std::string& path) {
    if (path.empty()) return false;

    // Create every missing component, ignoring the ones already there.
    for (size_t index = 1; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') continue;
        const std::string component = path.substr(0, index);
        if (component.empty() || component == "/") continue;
        if (mkdir(component.c_str(), 0775) != 0) {
            struct stat info {};
            // Anything other than "it already exists as a directory" is fatal
            // for this candidate.
            if (stat(component.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
        }
    }

    return access(path.c_str(), W_OK) == 0;
}

Layout choose(const std::vector<std::string>& candidates) {
    Layout layout;

    for (const std::string& candidate : candidates) {
        if (candidate.empty()) continue;
        if (!ensure_directory(candidate)) continue;

        const std::string mods = candidate + "/mods";
        const std::string config = candidate + "/config";
        if (!ensure_directory(mods) || !ensure_directory(config)) continue;

        layout.valid = true;
        layout.root = candidate;
        layout.mods = mods;
        layout.config = config;
        layout.log_file = candidate + "/loader.log";
        break;
    }

    return layout;
}

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_directory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

void collect_from(const std::string& directory, const std::string& extension,
                  std::vector<std::string>& out, bool descend) {
    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr) return;

    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string full = directory + "/" + name;
        if (is_directory(full)) {
            if (descend) collect_from(full, extension, out, false);
        } else if (ends_with(name, extension)) {
            out.push_back(full);
        }
    }
    closedir(handle);
}

}  // namespace

std::vector<std::string> collect_mod_files(const std::string& mods_directory,
                                           const std::string& extension) {
    std::vector<std::string> files;
    // One level only: a mod's directory holds its own files, not more mods.
    collect_from(mods_directory, extension, files, true);
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> default_candidates() {
    std::vector<std::string> candidates;

    // Lets a test or a curious user point the loader somewhere else.
    if (const char* override_path = getenv("MCBE_LOADER_DIR")) {
        if (*override_path != '\0') candidates.emplace_back(override_path);
    }

    // Visible to any file manager, which is the whole point: no developer
    // tools needed to read the log or drop in a mod.
    candidates.emplace_back("/sdcard/NRZLoader");
    candidates.emplace_back("/storage/emulated/0/NRZLoader");

    // The name this project used to ship under, so an existing install keeps
    // finding its mods after an update.
    candidates.emplace_back("/sdcard/MCPELoader");

    // Always writable by the app itself, but reaching it from a file manager
    // needs extra access on recent Android versions.
    candidates.emplace_back("/sdcard/Android/data/com.mojang.minecraftpe/files/NRZLoader");
    candidates.emplace_back("/data/data/com.mojang.minecraftpe/files/NRZLoader");

    return candidates;
}

}  // namespace mcbe::paths
