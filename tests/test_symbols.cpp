// Exercises symbol discovery against a stand-in game library, including the
// case where the library lives inside an APK and was never extracted to disk.

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/elf_image.h"
#include "core/symbols.h"

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
    if (argc < 3) {
        fprintf(stderr, "usage: %s <path to libfakegame.so> <path to archive.zip>\n", argv[0]);
        return 2;
    }
    const std::string library_path = argv[1];
    const std::string archive_path = argv[2];

    void* handle = dlopen(library_path.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }

    printf("symbol resolution\n");

    mcbe::SymbolResolver resolver;
    check(resolver.initialise("libfakegame.so"), "module located via dl_iterate_phdr");
    check(resolver.module().load_bias != 0, "load bias reported");
    check(!resolver.module().executable.empty(), "executable ranges reported");

    // Exported member function: must agree with what the dynamic linker says.
    void* expected_tick = dlsym(handle, "_ZN5Level4tickEv");
    void* resolved_tick = resolver.resolve("_ZN5Level4tickEv");
    check(resolved_tick != nullptr, "exported symbol Level::tick resolved");
    check(resolved_tick == expected_tick, "resolved address matches dlsym");

    void* resolved_hurt = resolver.resolve("_ZN5Actor4hurtEPKvfbb");
    check(resolved_hurt != nullptr, "exported symbol Actor::hurt resolved");

    // Local symbol: dlsym cannot see it, the ELF symbol table can.
    check(dlsym(handle, "_ZL16internal_computei") == nullptr, "local symbol is invisible to dlsym");
    void* resolved_local = resolver.resolve("_ZL16internal_computei");
    check(resolved_local != nullptr, "local symbol resolved from the symbol table");
    if (resolved_local != nullptr) {
        const int value = reinterpret_cast<int (*)(int)>(resolved_local)(5);
        check(value == 82, "local symbol address is callable and correct");
    }

    check(resolver.resolve("_ZN5Level11doesNotExistEv") == nullptr, "missing symbol returns null");

    // Signature scan: match the first bytes of a function we already located.
    if (resolved_tick != nullptr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(resolved_tick);
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "%02X %02X %02X %02X ?? ?? ?? ??", bytes[0], bytes[1],
                 bytes[2], bytes[3]);
        void* found = resolver.scan_signature(pattern);
        check(found != nullptr, "signature scan finds a match");
    }

    printf("apk embedded library\n");

    // The Android linker maps libraries straight out of the APK and reports
    // the path with a `!/` separator.
    const std::string embedded = archive_path + "!/lib/arm64-v8a/libfakegame.so";
    mcbe::ElfImage* from_zip = mcbe::ElfImage::open(embedded);
    check(from_zip != nullptr, "ELF parsed from inside the archive");

    mcbe::ElfImage* from_disk = mcbe::ElfImage::open(library_path);
    check(from_disk != nullptr, "ELF parsed from disk");

    if (from_zip != nullptr && from_disk != nullptr) {
        const uint64_t zip_offset = from_zip->symbol_offset("_ZN5Level4tickEv");
        const uint64_t disk_offset = from_disk->symbol_offset("_ZN5Level4tickEv");
        check(zip_offset != 0, "symbol found through the archive path");
        check(zip_offset == disk_offset, "archive and on-disk symbol values agree");
        check(from_zip->symbol_count() == from_disk->symbol_count(), "symbol counts agree");

        const auto matches = from_disk->find_containing("Level");
        check(matches.size() >= 2, "substring search lists the Level members");
    }

    delete from_zip;
    delete from_disk;

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
