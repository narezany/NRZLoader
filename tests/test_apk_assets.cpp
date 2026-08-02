// Reading interface files back out of the game package is what lets the
// loader build its menu from whatever the installed version actually ships,
// so both the stored and the compressed case are worth pinning down.

#include <cstdio>
#include <string>

#include "core/apk_assets.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to test.apk>\n", argv[0]);
        return 2;
    }

    printf("package reading\n");

    mcbe::ApkAssets assets;
    check(mcbe::ApkAssets::open(argv[1], assets), "package opened");

    // The linker reports libraries with this suffix; it must be ignored.
    mcbe::ApkAssets via_library_path;
    check(mcbe::ApkAssets::open(std::string(argv[1]) + "!/lib/arm64-v8a/libminecraftpe.so",
                                via_library_path),
          "the !/ form resolves to the same package");

    check(assets.contains("assets/ui/start_screen.json"), "known entry found");
    check(!assets.contains("assets/ui/nope.json"), "unknown entry not found");

    printf("\ncontents\n");

    bool ok = false;
    const std::string stored = assets.read("assets/ui/stored.json", &ok);
    check(ok, "stored entry read");
    check(stored == "{\"stored\":true}", "stored contents match");

    const std::string deflated = assets.read("assets/ui/start_screen.json", &ok);
    check(ok, "compressed entry read");
    check(deflated.size() == 20000, "compressed entry has the right length");
    check(deflated.compare(0, 21, "{\"namespace\":\"start\",") == 0, "decompressed contents match");
    check(deflated.find("\"play_button\"") != std::string::npos, "payload survived intact");

    const std::string missing = assets.read("assets/ui/nope.json", &ok);
    check(!ok, "missing entry reports failure");
    check(missing.empty(), "missing entry returns nothing");

    printf("\nsearch\n");
    check(assets.find_ending_with("ui/start_screen.json") == "assets/ui/start_screen.json",
          "suffix search finds the entry");
    check(assets.find_ending_with("ui/absent.json").empty(), "suffix search misses cleanly");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
