.text
.intel_syntax noprefix

// Distinct results at the same guest VA expose stale shared/local translations.
.globl fixture_constant_a
.globl fixture_constant_a_end
fixture_constant_a:
    mov     eax, 17
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_constant_a_end:

.globl fixture_constant_b
.globl fixture_constant_b_end
fixture_constant_b:
    mov     eax, 34
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_constant_b_end:

// Guest fixtures for the V0 CPU acceptance cases, assembled from source rather than hand-encoded.
//
// Hand-encoding is what produced the R2 defect in the 2026-09-08 review: `49 01 C8` was written
// intending `add r8, r9` but actually encodes `add r8, rcx`, so the case could not have passed even
// with a perfectly working backend. Every byte here is produced by the assembler and the extracted
// bytes are disassembled again at build time, so a wrong instruction fails the build instead of
// becoming a misleading test result.
//
// Calling convention for all fixtures: the harness seeds registers, then jumps to the fixture's
// first byte. Each fixture ends by loading the return-gate address into r15 and jumping to it.
// r15 is reserved for that purpose and no fixture may use it for a result, because the harness
// patches the gate address into the `movabs` immediately before running.
//
// Each fixture is emitted into its own section so the extractor can pull them out individually and
// the harness can place them at separate guest addresses.

// ---------------------------------------------------------------------------
// C01: integer arithmetic. Verifies add/mov/xor/sub and that r8..r11 are
// distinguishable from the lower-numbered registers, which is exactly what the
// mis-encoded original could not show.
// Inputs:  r8 = 0x1000, r9 = 0x234
// Expect:  r8 = 0x1234, r10 = 0x122F, r11 = 0
// ---------------------------------------------------------------------------
.globl fixture_integer
.globl fixture_integer_end
fixture_integer:
    add     r8, r9
    mov     r10, r8
    xor     r11, r11
    sub     r10, 5
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_integer_end:

// ---------------------------------------------------------------------------
// C01: conditional branch, both directions from one fixture.
// Inputs:  rdi, rsi
// Expect:  rax = 1 when rdi == rsi (branch not taken), 0 otherwise
// ---------------------------------------------------------------------------
.globl fixture_branch
.globl fixture_branch_end
fixture_branch:
    xor     rax, rax
    cmp     rdi, rsi
    jne     1f
    mov     rax, 1
1:
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_branch_end:

// ---------------------------------------------------------------------------
// C01: load/store through the guest stack.
// Inputs:  rdi, rsi
// Expect:  rax = rsi, rcx = rdi, rsp unchanged, and both values observable in
//          guest memory below the original rsp.
// ---------------------------------------------------------------------------
.globl fixture_loadstore
.globl fixture_loadstore_end
fixture_loadstore:
    push    rdi
    push    rsi
    pop     rax
    pop     rcx
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_loadstore_end:

// ---------------------------------------------------------------------------
// C01: a store that stays in memory, so a register-only failure can be told
// apart from a memory failure. Writes rdi to [rsp - 16] and leaves it there.
//
// Also reads both slots back into rax/rcx before returning. Without the readback
// a store whose result nothing consumes could in principle be eliminated, and
// the case could not tell "the store was optimised away" from "the store did not
// reach memory". With it, rax/rcx carry what the guest itself observed.
// ---------------------------------------------------------------------------
.globl fixture_store_memory
.globl fixture_store_memory_end
fixture_store_memory:
    mov     qword ptr [rsp - 16], rdi
    mov     qword ptr [rsp - 8], rsi
    mov     rax, qword ptr [rsp - 16]
    mov     rcx, qword ptr [rsp - 8]
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_store_memory_end:

// ---------------------------------------------------------------------------
// C02: SSE2 packed integer add plus a GPR round trip.
// Inputs:  rdi, rsi
// Expect:  xmm0 low = rdi + rsi packed as two 32-bit lanes, rax = same value
// ---------------------------------------------------------------------------
.globl fixture_sse2
.globl fixture_sse2_end
fixture_sse2:
    movq    xmm0, rdi
    movq    xmm1, rsi
    paddq   xmm0, xmm1
    movq    rax, xmm0
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_sse2_end:

// ---------------------------------------------------------------------------
// D05: a HLT that is NOT the registered return gate. Reaching this must not be
// reported as a normal return, or a crashed guest is indistinguishable from a
// finished one.
// ---------------------------------------------------------------------------
.globl fixture_bare_hlt
.globl fixture_bare_hlt_end
fixture_bare_hlt:
    hlt
fixture_bare_hlt_end:

// ---------------------------------------------------------------------------
// A fixture that only returns. Used where the harness needs a runnable thread
// without executing anything interesting.
// ---------------------------------------------------------------------------
.globl fixture_return_only
.globl fixture_return_only_end
fixture_return_only:
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_return_only_end:
