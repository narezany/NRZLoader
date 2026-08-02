// A mod says which loader it needs and the user says whether it runs at all.
// Both answers decide whether code gets executed, so both are worth testing
// away from a phone.

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "core/mod_manifest.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++g_failures;
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string workspace = argc > 1 ? argv[1] : "/tmp/mcbe-mods";

    const std::string mods = workspace + "/mods";
    const std::string config = workspace + "/config";
    const std::string package = mods + "/example";

    mkdir(workspace.c_str(), 0775);
    mkdir(mods.c_str(), 0775);
    mkdir(config.c_str(), 0775);
    mkdir(package.c_str(), 0775);

    using namespace mcbe::mods;

    printf("version comparison\n");
    check(compare_versions("1.0.0", "1.0.0") == 0, "equal versions");
    check(compare_versions("1.2", "1.2.0") == 0, "a missing part counts as zero");
    check(compare_versions("1.10.0", "1.9.0") > 0, "ten sorts above nine, not below it");
    check(compare_versions("1.0.0", "1.1.0") < 0, "a newer minor wins");
    check(compare_versions("2.0", "1.99.99") > 0, "a newer major wins");
    check(compare_versions("", "0.0.0") == 0, "an empty version is zero");
    check(compare_versions("1.x", "1.0") == 0, "junk counts as zero rather than throwing");

    printf("\nreading a manifest\n");
    write_file(package + "/mod.json", R"({
  "id": "nrz.example",
  "name": "Example",
  "version": "2.0.0",
  "minLoader": "1.1.0",
  "maxLoader": "2.0.0"
})");

    const Manifest manifest = read_manifest(package);
    check(manifest.found, "the manifest was found");
    check(manifest.id == "nrz.example", "the id was read");
    check(manifest.name == "Example", "the name was read");
    check(manifest.min_loader == "1.1.0", "the lower bound was read");
    check(manifest.max_loader == "2.0.0", "the upper bound was read");
    check(!read_manifest(mods + "/nothing").found, "a missing manifest is not an error");

    printf("\nwhich id a file is known by\n");
    check(id_for(package + "/main.js", mods) == "nrz.example", "a packaged file takes the mod's id");
    check(id_for(mods + "/loose.js", mods) == "loose.js", "a loose file is known by its name");

    printf("\nversion requirements\n");
    std::string reason;
    check(should_run(package + "/main.js", mods, config, "1.1.0", reason),
          "the lower bound exactly met");
    check(should_run(package + "/main.js", mods, config, "1.5.0", reason), "inside the range");
    check(!should_run(package + "/main.js", mods, config, "1.0.0", reason), "below the range");
    check(reason.find("1.1.0") != std::string::npos, "and the reason names what it needs");
    check(!should_run(package + "/main.js", mods, config, "2.1.0", reason), "above the range");
    check(should_run(mods + "/loose.js", mods, config, "0.0.1", reason),
          "a file with no manifest is not held to a version");

    printf("\nswitched off by the user\n");
    write_file(config + "/disabled.txt",
               "# mods the user turned off\n"
               "nrz.example\n"
               "\n"
               "loose.js  \n");

    check(disabled_ids(config).size() == 2, "comments and blank lines are skipped");
    check(!should_run(package + "/main.js", mods, config, "1.1.0", reason),
          "a switched-off package does not run");
    check(reason == "switched off", "and says so plainly");
    check(!should_run(mods + "/loose.js", mods, config, "1.1.0", reason),
          "a switched-off loose file does not run either");

    write_file(config + "/disabled.txt", "");
    check(should_run(package + "/main.js", mods, config, "1.1.0", reason),
          "emptying the list switches it back on");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
