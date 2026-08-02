// ARM64 instruction relocator.
//
// When we overwrite the first bytes of a function with a jump, the original
// instructions have to keep working from a new address. Most AArch64
// instructions are position independent and can simply be copied, but a
// handful encode a PC-relative offset and must be rewritten.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mcbe::hook {

enum class RelocStatus {
    Ok = 0,
    UnsupportedInstruction,  // PC-relative form we refuse to guess at
    OutOfSpace,              // destination buffer too small
};

// Number of words in an absolute jump sequence (ldr x17, #8 / br x17 / .quad).
inline constexpr size_t kAbsJumpWords = 4;

// Worst case output for a single input instruction: a BL expands into a
// four word constant load for x30 plus a four word absolute jump.
inline constexpr size_t kMaxWordsPerInsn = 8;

struct RelocResult {
    RelocStatus status = RelocStatus::Ok;
    size_t words_written = 0;
    // Index of the instruction that failed, valid when status != Ok.
    size_t failed_index = 0;
};

// Writes an absolute jump to `target` at `out`. Returns words written.
// Clobbers x17, which AAPCS64 reserves as an inter-procedure scratch register
// (IP1) and is therefore never live at a function entry point.
size_t emit_abs_jump(uint32_t* out, uint64_t target);

// Relocates `count` instructions read from `src` (which is executing at
// `src_pc`) into `out` (which will execute at `out_pc`).
//
// `out_capacity` is counted in 32-bit words. On failure nothing is guaranteed
// about the contents of `out`; the caller should discard the buffer.
RelocResult relocate(const uint32_t* src, uint64_t src_pc, uint32_t* out, uint64_t out_pc,
                     size_t count, size_t out_capacity);

// True when the instruction encodes a PC-relative operand that `relocate`
// knows how to rewrite. Exposed for tests.
bool is_pc_relative(uint32_t insn);

}  // namespace mcbe::hook
