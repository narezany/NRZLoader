// Functions whose first four instructions exercise every PC-relative form the
// relocator has to rewrite. Each returns a value that only comes out right if
// the displaced prologue kept working from its new address.

    .text

    .global p_plain
    .type p_plain, %function
p_plain:
    mov     w1, #10
    add     w0, w0, w1
    mov     w2, #5
    sub     w0, w0, w2
    ret
    .size p_plain, . - p_plain

    .global p_adrp
    .type p_adrp, %function
p_adrp:
    adrp    x1, g_value
    add     x1, x1, :lo12:g_value
    ldr     w1, [x1]
    add     w0, w0, w1
    ret
    .size p_adrp, . - p_adrp

    .global p_adr
    .type p_adr, %function
p_adr:
    adr     x1, .Ladr_data
    ldr     w1, [x1]
    nop
    add     w0, w0, w1
    ret
    .balign 8
.Ladr_data:
    .word   9
    .size p_adr, . - p_adr

    .global p_ldr_literal
    .type p_ldr_literal, %function
p_ldr_literal:
    ldr     x1, .Lliteral
    nop
    nop
    add     w0, w0, w1
    ret
    .balign 8
.Lliteral:
    .quad   7
    .size p_ldr_literal, . - p_ldr_literal

    .global p_ldr_literal_fp
    .type p_ldr_literal_fp, %function
p_ldr_literal_fp:
    ldr     d0, .Lfp_literal
    nop
    nop
    fcvtzs  w1, d0
    add     w0, w0, w1
    ret
    .balign 8
.Lfp_literal:
    .double 3.0
    .size p_ldr_literal_fp, . - p_ldr_literal_fp

    .global p_branch
    .type p_branch, %function
p_branch:
    mov     w1, #3
    b       .Lbranch_target
    nop
    nop
.Lbranch_target:
    add     w0, w0, w1
    ret
    .size p_branch, . - p_branch

    .global p_call
    .type p_call, %function
p_call:
    stp     x29, x30, [sp, #-16]!
    bl      .Lcallee
    ldp     x29, x30, [sp], #16
    add     w0, w0, w1
    ret
.Lcallee:
    mov     w1, #42
    ret
    .size p_call, . - p_call

    .global p_cbz
    .type p_cbz, %function
p_cbz:
    cbz     w0, .Lcbz_zero
    mov     w1, #100
    add     w0, w0, w1
    nop
    ret
.Lcbz_zero:
    mov     w0, #-1
    ret
    .size p_cbz, . - p_cbz

    .global p_tbz
    .type p_tbz, %function
p_tbz:
    tbz     w0, #0, .Ltbz_even
    mov     w0, #1000
    nop
    nop
    ret
.Ltbz_even:
    mov     w0, #2000
    ret
    .size p_tbz, . - p_tbz

    .global p_bcond
    .type p_bcond, %function
p_bcond:
    cmp     w0, #5
    b.gt    .Lbcond_big
    mov     w0, #1
    nop
    ret
.Lbcond_big:
    mov     w0, #2
    ret
    .size p_bcond, . - p_bcond

    .data
    .global g_value
    .balign 4
g_value:
    .word   1234
