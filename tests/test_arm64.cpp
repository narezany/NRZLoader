// Runs the relocator and the inline hooker against real AArch64 code.
//
// Two independent checks per function:
//   relocation  - copy the prologue elsewhere and confirm behaviour survives;
//   hooking     - install a detour and confirm both the detour and the
//                 trampoline path produce the expected results.

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../src/hook/arm64_reloc.h"
#include "../src/hook/inline_hook.h"

extern "C" {
int p_plain(int);
int p_adrp(int);
int p_adr(int);
int p_ldr_literal(int);
int p_ldr_literal_fp(int);
int p_branch(int);
int p_call(int);
int p_cbz(int);
int p_tbz(int);
int p_bcond(int);
}

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        printf("  ok    %s\n", what.c_str());
    } else {
        printf("  FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

using IntFn = int (*)(int);

// Copies the first four instructions of `function` into a fresh executable
// page and appends a jump back, producing a standalone clone.
IntFn clone_prologue(IntFn function, const std::string& name) {
    using namespace mcbe::hook;

    const size_t capacity = 4 * kMaxWordsPerInsn + kAbsJumpWords;
    const size_t size = 4096;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return nullptr;

    const auto source_pc = reinterpret_cast<uint64_t>(function);
    const auto destination_pc = reinterpret_cast<uint64_t>(memory);

    const RelocResult result = relocate(reinterpret_cast<const uint32_t*>(function), source_pc,
                                        static_cast<uint32_t*>(memory), destination_pc, 4, capacity);
    if (result.status != RelocStatus::Ok) {
        printf("  FAIL  %s: relocation refused at instruction %zu\n", name.c_str(),
               result.failed_index);
        ++g_failures;
        ++g_checks;
        return nullptr;
    }

    emit_abs_jump(static_cast<uint32_t*>(memory) + result.words_written, source_pc + 16);

    auto* bytes = static_cast<char*>(memory);
    __builtin___clear_cache(bytes, bytes + size);
    if (mprotect(memory, size, PROT_READ | PROT_EXEC) != 0) return nullptr;
    return reinterpret_cast<IntFn>(memory);
}

void test_relocation(IntFn function, const std::string& name, const std::vector<int>& inputs) {
    IntFn clone = clone_prologue(function, name);
    if (clone == nullptr) return;

    bool all_equal = true;
    for (int input : inputs) {
        const int expected = function(input);
        const int actual = clone(input);
        if (expected != actual) {
            printf("        %s(%d): expected %d, relocated copy returned %d\n", name.c_str(), input,
                   expected, actual);
            all_equal = false;
        }
    }
    check(all_equal, "relocated prologue matches original: " + name);
}

// The detour records that it ran and forwards to the original function.
mcbe::hook::InlineHook g_hook;
int g_detour_calls = 0;

int detour(int value) {
    ++g_detour_calls;
    auto original = g_hook.original<IntFn>();
    return original(value) + 1000000;
}

void test_hooking(IntFn function, const std::string& name, int input) {
    const int before = function(input);

    g_detour_calls = 0;
    const auto status = g_hook.install(reinterpret_cast<void*>(function),
                                       reinterpret_cast<void*>(&detour));
    if (status != mcbe::hook::HookStatus::Ok) {
        check(false, name + ": install failed (" + mcbe::hook::status_string(status) + ")");
        return;
    }

    const int hooked = function(input);
    check(g_detour_calls == 1, name + ": detour ran");
    check(hooked == before + 1000000, name + ": trampoline returned the original result");

    const auto removed = g_hook.remove();
    check(removed == mcbe::hook::HookStatus::Ok, name + ": hook removed");
    check(function(input) == before, name + ": behaviour restored after removal");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("relocation\n");
    test_relocation(p_plain, "p_plain", {0, 1, 7});
    test_relocation(p_adrp, "p_adrp", {0, 5});
    test_relocation(p_adr, "p_adr", {0, 5});
    test_relocation(p_ldr_literal, "p_ldr_literal", {0, 5});
    test_relocation(p_ldr_literal_fp, "p_ldr_literal_fp", {0, 5});
    test_relocation(p_branch, "p_branch", {0, 5});
    test_relocation(p_call, "p_call", {0, 5});
    test_relocation(p_cbz, "p_cbz", {0, 1, 5});
    test_relocation(p_tbz, "p_tbz", {0, 1, 2, 3});
    test_relocation(p_bcond, "p_bcond", {0, 5, 9});

    printf("hooking\n");
    test_hooking(p_plain, "p_plain", 7);
    test_hooking(p_adrp, "p_adrp", 7);
    test_hooking(p_adr, "p_adr", 7);
    test_hooking(p_ldr_literal, "p_ldr_literal", 7);
    test_hooking(p_ldr_literal_fp, "p_ldr_literal_fp", 7);
    test_hooking(p_branch, "p_branch", 7);
    test_hooking(p_call, "p_call", 7);
    test_hooking(p_cbz, "p_cbz", 3);
    test_hooking(p_tbz, "p_tbz", 3);
    test_hooking(p_bcond, "p_bcond", 9);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
