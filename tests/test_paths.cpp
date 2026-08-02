// The loader has to survive a device where the obvious locations are not
// writable, so the fallback order is worth testing directly.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

#include "core/paths.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++g_failures;
}

bool is_directory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string workspace = argc > 1 ? argv[1] : "/tmp/mcbe-paths";

    printf("directory layout\n");

    const std::string good = workspace + "/writable/MCPELoader";
    const std::string nested = workspace + "/deep/a/b/c";

    check(mcbe::paths::ensure_directory(good), "creates a directory");
    check(is_directory(good), "the directory really exists");
    check(mcbe::paths::ensure_directory(good), "creating an existing one is fine");
    check(mcbe::paths::ensure_directory(nested), "creates missing parents");
    check(is_directory(nested), "the nested path really exists");

    // A path under a file can never become a directory.
    const std::string blocker = workspace + "/blocker";
    FILE* handle = fopen(blocker.c_str(), "w");
    if (handle != nullptr) fclose(handle);
    check(!mcbe::paths::ensure_directory(blocker + "/child"), "refuses a path blocked by a file");

    printf("\nfallback order\n");

    const std::string unwritable = "/proc/definitely/not/here";
    mcbe::paths::Layout layout = mcbe::paths::choose({unwritable, good});
    check(layout.valid, "falls back past an unusable candidate");
    check(layout.root == good, "picks the first candidate that works");
    check(is_directory(layout.mods), "creates mods/");
    check(is_directory(layout.config), "creates config/");
    check(layout.log_file == good + "/loader.log", "log file sits at the root");

    layout = mcbe::paths::choose({unwritable, blocker + "/child"});
    check(!layout.valid, "reports failure when nothing is usable");

    printf("\nmod collection\n");
    const std::string mods = workspace + "/mods";
    mcbe::paths::ensure_directory(mods + "/packaged.mod");
    mcbe::paths::ensure_directory(mods + "/other.mod/nested");

    auto touch = [](const std::string& path) {
        FILE* file = fopen(path.c_str(), "w");
        if (file != nullptr) fclose(file);
    };
    touch(mods + "/loose.js");
    touch(mods + "/loose.so");
    touch(mods + "/notes.txt");
    touch(mods + "/packaged.mod/main.js");
    touch(mods + "/packaged.mod/mod.json");
    touch(mods + "/other.mod/native.so");
    touch(mods + "/other.mod/nested/deep.js");

    const auto scripts = mcbe::paths::collect_mod_files(mods, ".js");
    const auto natives = mcbe::paths::collect_mod_files(mods, ".so");

    check(scripts.size() == 2, "loose and packaged scripts both found");
    check(natives.size() == 2, "loose and packaged libraries both found");
    check(std::find(scripts.begin(), scripts.end(), mods + "/loose.js") != scripts.end(),
          "a loose script is picked up");
    check(std::find(scripts.begin(), scripts.end(), mods + "/packaged.mod/main.js") != scripts.end(),
          "a script inside a package is picked up");
    check(std::find(scripts.begin(), scripts.end(), mods + "/other.mod/nested/deep.js") == scripts.end(),
          "nothing deeper than a package is picked up");
    check(std::is_sorted(scripts.begin(), scripts.end()), "order is stable");

    printf("\ncandidate list\n");
    setenv("MCBE_LOADER_DIR", "/custom/place", 1);
    const auto candidates = mcbe::paths::default_candidates();
    check(!candidates.empty() && candidates.front() == "/custom/place",
          "the override is tried first");
    check(candidates.size() >= 4, "device locations are present as fallbacks");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
