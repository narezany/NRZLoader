#include "arm64_reloc.h"

namespace mcbe::hook {
namespace {

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

constexpr uint32_t kScratch = 17;  // x17 / IP1

int64_t sign_extend(uint64_t value, unsigned bits) {
    const uint64_t sign_bit = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign_bit) - sign_bit);
}

uint32_t movz(uint32_t rd, uint16_t imm16, unsigned shift) {
    return 0xD2800000u | ((shift / 16) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd;
}

uint32_t movk(uint32_t rd, uint16_t imm16, unsigned shift) {
    return 0xF2800000u | ((shift / 16) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd;
}

// Materialises a full 64-bit constant into `rd` using a fixed four instruction
// sequence. Always four words so output sizes stay predictable.
size_t emit_load_const(uint32_t* out, uint32_t rd, uint64_t value) {
    out[0] = movz(rd, static_cast<uint16_t>(value & 0xFFFF), 0);
    out[1] = movk(rd, static_cast<uint16_t>((value >> 16) & 0xFFFF), 16);
    out[2] = movk(rd, static_cast<uint16_t>((value >> 32) & 0xFFFF), 32);
    out[3] = movk(rd, static_cast<uint16_t>((value >> 48) & 0xFFFF), 48);
    return 4;
}

// Unconditional branch to a nearby word offset, used to skip over a block.
uint32_t branch_words(int64_t words) {
    return 0x14000000u | (static_cast<uint32_t>(words) & 0x03FFFFFFu);
}

// ---------------------------------------------------------------------------
// Instruction classification
// ---------------------------------------------------------------------------

bool is_adr(uint32_t i) { return (i & 0x9F000000u) == 0x10000000u; }
bool is_adrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }
bool is_b(uint32_t i) { return (i & 0xFC000000u) == 0x14000000u; }
bool is_bl(uint32_t i) { return (i & 0xFC000000u) == 0x94000000u; }
bool is_b_cond(uint32_t i) { return (i & 0xFF000010u) == 0x54000000u; }
bool is_cb(uint32_t i) { return (i & 0x7E000000u) == 0x34000000u; }   // CBZ/CBNZ
bool is_tb(uint32_t i) { return (i & 0x7E000000u) == 0x36000000u; }   // TBZ/TBNZ
// The mask has to cover bit 26 (the V field), otherwise an integer literal
// load and a SIMD one are indistinguishable and the wrong register gets
// written.
bool is_ldr_lit_int(uint32_t i) { return (i & 0x3F000000u) == 0x18000000u; }
bool is_ldr_lit_fp(uint32_t i) { return (i & 0x3F000000u) == 0x1C000000u; }

}  // namespace

bool is_pc_relative(uint32_t i) {
    return is_adr(i) || is_adrp(i) || is_b(i) || is_bl(i) || is_b_cond(i) || is_cb(i) ||
           is_tb(i) || is_ldr_lit_int(i) || is_ldr_lit_fp(i);
}

size_t emit_abs_jump(uint32_t* out, uint64_t target) {
    out[0] = 0x58000051u;  // ldr x17, #8
    out[1] = 0xD61F0220u;  // br  x17
    out[2] = static_cast<uint32_t>(target & 0xFFFFFFFFu);
    out[3] = static_cast<uint32_t>(target >> 32);
    return kAbsJumpWords;
}

RelocResult relocate(const uint32_t* src, uint64_t src_pc, uint32_t* out, uint64_t out_pc,
                     size_t count, size_t out_capacity) {
    RelocResult result;
    size_t written = 0;

    auto room_for = [&](size_t words) { return written + words <= out_capacity; };

    for (size_t index = 0; index < count; ++index) {
        const uint32_t insn = src[index];
        const uint64_t pc = src_pc + index * 4;
        uint32_t* dst = out + written;

        // A conditional branch is turned into: take the branch over a short
        // hop, then an absolute jump to the original destination. The
        // not-taken path skips the whole block and continues inline.
        auto emit_conditional = [&](uint32_t rewritten_cond, uint64_t target) -> bool {
            if (!room_for(2 + kAbsJumpWords)) return false;
            dst[0] = rewritten_cond;                                  // cond -> +2 words
            // Not taken: hop over the absolute jump block that follows.
            dst[1] = branch_words(1 + static_cast<int64_t>(kAbsJumpWords));
            emit_abs_jump(dst + 2, target);
            written += 2 + kAbsJumpWords;
            return true;
        };

        if (is_b(insn) || is_bl(insn)) {
            const uint64_t target = pc + static_cast<uint64_t>(sign_extend(insn & 0x03FFFFFFu, 26) * 4);
            if (is_bl(insn)) {
                // Preserve call semantics: x30 must point at the instruction
                // that follows, which now lives in the trampoline.
                const size_t seq = 4 + kAbsJumpWords;
                if (!room_for(seq)) { result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result; }
                const uint64_t return_addr = out_pc + (written + seq) * 4;
                emit_load_const(dst, 30, return_addr);
                out[written + 4] = 0x58000051u;  // ldr x17, #8
                out[written + 5] = 0xD61F0220u;  // br  x17 (no link: x30 preset)
                out[written + 6] = static_cast<uint32_t>(target & 0xFFFFFFFFu);
                out[written + 7] = static_cast<uint32_t>(target >> 32);
                written += seq;
            } else {
                if (!room_for(kAbsJumpWords)) { result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result; }
                emit_abs_jump(dst, target);
                written += kAbsJumpWords;
            }
            continue;
        }

        if (is_b_cond(insn)) {
            const uint64_t target = pc + static_cast<uint64_t>(sign_extend((insn >> 5) & 0x7FFFFu, 19) * 4);
            // Same condition, but branching two words forward.
            const uint32_t rewritten = (insn & 0xFF00001Fu) | (2u << 5);
            if (!emit_conditional(rewritten, target)) {
                result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result;
            }
            continue;
        }

        if (is_cb(insn)) {
            const uint64_t target = pc + static_cast<uint64_t>(sign_extend((insn >> 5) & 0x7FFFFu, 19) * 4);
            const uint32_t rewritten = (insn & 0xFF00001Fu) | (2u << 5);
            if (!emit_conditional(rewritten, target)) {
                result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result;
            }
            continue;
        }

        if (is_tb(insn)) {
            const uint64_t target = pc + static_cast<uint64_t>(sign_extend((insn >> 5) & 0x3FFFu, 14) * 4);
            // TBZ/TBNZ carry a 14-bit offset at the same position.
            const uint32_t rewritten = (insn & 0xFFF8001Fu) | (2u << 5);
            if (!emit_conditional(rewritten, target)) {
                result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result;
            }
            continue;
        }

        if (is_adr(insn) || is_adrp(insn)) {
            const uint32_t rd = insn & 0x1Fu;
            const uint64_t imm = ((insn >> 5) & 0x7FFFFu) << 2 | ((insn >> 29) & 0x3u);
            uint64_t target;
            if (is_adr(insn)) {
                target = pc + static_cast<uint64_t>(sign_extend(imm, 21));
            } else {
                target = (pc & ~0xFFFull) + static_cast<uint64_t>(sign_extend(imm, 21) * 4096);
            }
            if (!room_for(4)) { result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result; }
            written += emit_load_const(dst, rd, target);
            continue;
        }

        if (is_ldr_lit_int(insn) || is_ldr_lit_fp(insn)) {
            const uint64_t addr = pc + static_cast<uint64_t>(sign_extend((insn >> 5) & 0x7FFFFu, 19) * 4);
            const uint32_t rt = insn & 0x1Fu;
            const uint32_t opc = (insn >> 30) & 0x3u;
            if (!room_for(5)) { result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result; }

            // The literal address always goes into x17 rather than into rt:
            // rt may be the zero register, or a prefetch operation rather than
            // a register at all.
            emit_load_const(dst, kScratch, addr);
            uint32_t load;
            if (is_ldr_lit_int(insn)) {
                switch (opc) {
                    case 0: load = 0xB9400000u; break;  // ldr  wt, [x17]
                    case 1: load = 0xF9400000u; break;  // ldr  xt, [x17]
                    case 2: load = 0xB9800000u; break;  // ldrsw xt, [x17]
                    default: load = 0xF9800000u; break; // prfm  op, [x17]
                }
            } else {
                switch (opc) {
                    case 0: load = 0xBD400000u; break;  // ldr st, [x17]
                    case 1: load = 0xFD400000u; break;  // ldr dt, [x17]
                    case 2: load = 0x3DC00000u; break;  // ldr qt, [x17]
                    default:
                        result.status = RelocStatus::UnsupportedInstruction;
                        result.failed_index = index;
                        return result;
                }
            }
            dst[4] = load | (kScratch << 5) | rt;
            written += 5;
            continue;
        }

        // Everything else on AArch64 is position independent.
        if (!room_for(1)) { result.status = RelocStatus::OutOfSpace; result.failed_index = index; return result; }
        dst[0] = insn;
        written += 1;
    }

    result.words_written = written;
    return result;
}

}  // namespace mcbe::hook
