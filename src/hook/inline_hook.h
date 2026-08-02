// Inline function hooking for AArch64.
//
// Installing a hook overwrites the first 16 bytes of the target with an
// absolute jump to the detour, and copies the displaced instructions into a
// trampoline so the original function stays callable.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mcbe::hook {

enum class HookStatus {
    Ok = 0,
    BadAddress,
    UnsupportedPrologue,  // the prologue contains something we won't rewrite
    ProtectFailed,
    AllocFailed,
    AlreadyHooked,
    NotHooked,
};

const char* status_string(HookStatus status);

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // Redirects `target` to `detour`. On success `original()` returns a
    // callable pointer that runs the untouched function.
    HookStatus install(void* target, void* detour);

    // Restores the original bytes. Safe to call from the destructor.
    HookStatus remove();

    bool installed() const { return installed_; }

    // Callable entry point for the unhooked behaviour. Null until installed.
    template <typename Fn>
    Fn original() const {
        return reinterpret_cast<Fn>(trampoline_);
    }

    void* original_raw() const { return trampoline_; }

private:
    void* target_ = nullptr;
    void* trampoline_ = nullptr;
    size_t trampoline_size_ = 0;
    uint32_t saved_[4] = {};
    bool installed_ = false;
};

// Overwrites entry `index` of the vtable that `object` points at, returning the
// previous entry through `out_original`. Virtual dispatch is a plain pointer
// load, so this is cheaper and far more robust than patching code.
HookStatus hook_vtable(void* object, size_t index, void* detour, void** out_original);

// Replaces an entry in a vtable given the table address directly.
HookStatus hook_vtable_at(void** vtable, size_t index, void* detour, void** out_original);

}  // namespace mcbe::hook
