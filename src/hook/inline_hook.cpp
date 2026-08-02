#include "inline_hook.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "arm64_reloc.h"

namespace mcbe::hook {
namespace {

// Four instructions are displaced by the absolute jump we write over them.
constexpr size_t kPatchWords = kAbsJumpWords;
constexpr size_t kPatchBytes = kPatchWords * 4;

size_t page_size() {
    static const size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

uintptr_t page_align(uintptr_t address) { return address & ~(page_size() - 1); }

// Toggles write permission on the pages covering [address, address + length).
//
// Asking for write and execute at once is what every hooking library does, but
// some SELinux policies refuse it for file backed pages. Falling back to a
// non-executable window keeps the patch working; the race it opens is narrow
// and only matters if another thread is inside these exact bytes.
bool set_writable(void* address, size_t length, bool writable) {
    const uintptr_t start = page_align(reinterpret_cast<uintptr_t>(address));
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + length;
    const size_t span = end - start;
    void* page = reinterpret_cast<void*>(start);

    if (!writable) return mprotect(page, span, PROT_READ | PROT_EXEC) == 0;
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) return true;
    return mprotect(page, span, PROT_READ | PROT_WRITE) == 0;
}

void flush_icache(void* address, size_t length) {
    char* begin = static_cast<char*>(address);
    __builtin___clear_cache(begin, begin + length);
}

// Trampolines are allocated writable and only then flipped to executable.
// Android's SELinux policy denies `execmem` to app processes, so a page that
// is simultaneously writable and executable would be rejected.
void* alloc_exec_page(size_t size) {
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
}

bool make_executable(void* memory, size_t size) {
    return mprotect(memory, size, PROT_READ | PROT_EXEC) == 0;
}

}  // namespace

const char* status_string(HookStatus status) {
    switch (status) {
        case HookStatus::Ok: return "ok";
        case HookStatus::BadAddress: return "bad address";
        case HookStatus::UnsupportedPrologue: return "unsupported prologue";
        case HookStatus::ProtectFailed: return "mprotect failed";
        case HookStatus::AllocFailed: return "trampoline allocation failed";
        case HookStatus::AlreadyHooked: return "already hooked";
        case HookStatus::NotHooked: return "not hooked";
    }
    return "unknown";
}

InlineHook::~InlineHook() {
    if (installed_) remove();
    if (trampoline_ != nullptr) munmap(trampoline_, trampoline_size_);
}

HookStatus InlineHook::install(void* target, void* detour) {
    if (installed_) return HookStatus::AlreadyHooked;
    if (target == nullptr || detour == nullptr) return HookStatus::BadAddress;
    if ((reinterpret_cast<uintptr_t>(target) & 3u) != 0) return HookStatus::BadAddress;

    const size_t capacity = kPatchWords * kMaxWordsPerInsn + kAbsJumpWords;
    const size_t bytes = capacity * 4;
    const size_t alloc_size = (bytes + page_size() - 1) & ~(page_size() - 1);

    void* trampoline = alloc_exec_page(alloc_size);
    if (trampoline == nullptr) return HookStatus::AllocFailed;

    const auto* source = static_cast<const uint32_t*>(target);
    auto* destination = static_cast<uint32_t*>(trampoline);
    const uint64_t source_pc = reinterpret_cast<uint64_t>(target);
    const uint64_t destination_pc = reinterpret_cast<uint64_t>(trampoline);

    const RelocResult reloc =
        relocate(source, source_pc, destination, destination_pc, kPatchWords, capacity);
    if (reloc.status != RelocStatus::Ok) {
        munmap(trampoline, alloc_size);
        return HookStatus::UnsupportedPrologue;
    }

    // Continue in the original function right after the displaced bytes.
    emit_abs_jump(destination + reloc.words_written, source_pc + kPatchBytes);
    flush_icache(trampoline, bytes);

    if (!make_executable(trampoline, alloc_size)) {
        munmap(trampoline, alloc_size);
        return HookStatus::ProtectFailed;
    }

    std::memcpy(saved_, target, kPatchBytes);

    if (!set_writable(target, kPatchBytes, true)) {
        munmap(trampoline, alloc_size);
        return HookStatus::ProtectFailed;
    }

    uint32_t patch[kPatchWords];
    emit_abs_jump(patch, reinterpret_cast<uint64_t>(detour));
    std::memcpy(target, patch, kPatchBytes);

    set_writable(target, kPatchBytes, false);
    flush_icache(target, kPatchBytes);

    target_ = target;
    trampoline_ = trampoline;
    trampoline_size_ = alloc_size;
    installed_ = true;
    return HookStatus::Ok;
}

HookStatus InlineHook::remove() {
    if (!installed_) return HookStatus::NotHooked;
    if (!set_writable(target_, kPatchBytes, true)) return HookStatus::ProtectFailed;

    std::memcpy(target_, saved_, kPatchBytes);

    set_writable(target_, kPatchBytes, false);
    flush_icache(target_, kPatchBytes);

    installed_ = false;
    return HookStatus::Ok;
}

HookStatus hook_vtable_at(void** vtable, size_t index, void* detour, void** out_original) {
    if (vtable == nullptr || detour == nullptr) return HookStatus::BadAddress;

    void** slot = &vtable[index];
    if (!set_writable(slot, sizeof(void*), true)) return HookStatus::ProtectFailed;

    if (out_original != nullptr) *out_original = *slot;
    *slot = detour;

    set_writable(slot, sizeof(void*), false);
    return HookStatus::Ok;
}

HookStatus hook_vtable(void* object, size_t index, void* detour, void** out_original) {
    if (object == nullptr) return HookStatus::BadAddress;
    void** vtable = *reinterpret_cast<void***>(object);
    return hook_vtable_at(vtable, index, detour, out_original);
}

}  // namespace mcbe::hook
