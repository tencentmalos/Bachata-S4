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

// ---------------------------------------------------------------------------
// R2-C02: a loop with no HLE call, no syscall, no yield and no natural exit.
//
// This is the fixture an asynchronous stop has to work against. A cooperative
// check cannot end it, and neither can waiting for a block boundary: once FEX
// has compiled the backward jump, the JIT stays inside a single translated
// block indefinitely. Only an out-of-band kick gets the owner out.
//
// The counter gives the test something to observe. A resumed loop must show it
// advancing, which distinguishes "resumed" from "restarted" and from "never
// actually stopped".
// ---------------------------------------------------------------------------
.globl fixture_spin_loop
.globl fixture_spin_loop_end
fixture_spin_loop:
    xor     rax, rax
spin_again:
    inc     rax
    jmp     spin_again
fixture_spin_loop_end:

// R2-C01/C02: aligned progress stores observable by the controller. RDI points
// to a distinct uint64_t for each owner. Neither HLE nor a guest yield is used.
.globl fixture_progress_loop
.globl fixture_progress_loop_end
fixture_progress_loop:
progress_again:
    inc     rax
    mov     qword ptr [rdi], rax
    jmp     progress_again
fixture_progress_loop_end:

// R2-M01/N1 persistent-owner version switch. Two versions of an aligned progress
// loop. Each stores a distinct version marker in the first qword at [rdi], and a
// advancing counter in the second qword at [rdi+8]. A controller that republished
// version B while the same two owners were paused must, on resume and re-dispatch,
// observe the marker flip and the counter keep advancing -- proving new code runs
// on the SAME native owner/handle, not a fresh thread.
.globl fixture_version_a_loop
.globl fixture_version_a_loop_end
fixture_version_a_loop:
    xor     rax, rax
version_a_again:
    inc     rax
    mov     rcx, 0x0A0A0A0A0A0A0A0A
    mov     qword ptr [rdi], rcx
    mov     qword ptr [rdi + 8], rax
    jmp     version_a_again
fixture_version_a_loop_end:

.globl fixture_version_b_loop
.globl fixture_version_b_loop_end
fixture_version_b_loop:
    xor     rax, rax
version_b_again:
    inc     rax
    mov     rcx, 0x0B0B0B0B0B0B0B0B
    mov     qword ptr [rdi], rcx
    mov     qword ptr [rdi + 8], rax
    jmp     version_b_again
fixture_version_b_loop_end:

// RDI = target code, ESI = this iteration's new immediate. Only the guest authors it.
.globl fixture_smc_writer
.globl fixture_smc_writer_end
fixture_smc_writer:
    mov     byte ptr [rdi], 0xB8
    mov     dword ptr [rdi + 1], esi
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_smc_writer_end:

// A separately cached caller for cross-block permission/unmap regressions.
.globl fixture_jump_target
.globl fixture_jump_target_end
fixture_jump_target:
    jmp     rdi
fixture_jump_target_end:

// R2-C04: observe owner-written GPR, XMM, MXCSR rounding, FS/GS and RIP.
// RDI=output; XMM1=1.75. MXCSR toward-zero must make CVTSD2SI produce 1.
.globl fixture_state_echo
.globl fixture_state_echo_end
fixture_state_echo:
    mov     qword ptr [rdi], rax
    movdqu  xmmword ptr [rdi + 8], xmm0
    stmxcsr dword ptr [rdi + 24]
    mov     rbx, qword ptr fs:[0]
    mov     qword ptr [rdi + 32], rbx
    mov     rbx, qword ptr gs:[0]
    mov     qword ptr [rdi + 40], rbx
    cvtsd2si rax, xmm1
    mov     qword ptr [rdi + 48], rax
    movabs  r15, 0x1122334455667788
    jmp     r15
fixture_state_echo_end:
