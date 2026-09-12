// Switching between a worker's own stack and a task's.
//
// sword_ctx_switch(void **save_sp, void *resume_sp) saves the callee-saved
// registers on the current stack, writes the resulting stack pointer through
// `save_sp`, then moves onto `resume_sp` and restores what is waiting there.
// It returns on whichever stack it landed on, which is the whole trick: the
// call appears to return twice, once per side.
//
// Only callee-saved registers are touched. Everything else is already the
// caller's problem at a call boundary, and a switch is a call.

#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

.text

#if defined(__aarch64__) || defined(__arm64__)

// x0 = save_sp, x1 = resume_sp. AAPCS keeps x19-x28, the frame pointer, the
// link register and the low halves of v8-v15.
.globl SYM(sword_ctx_switch)
.p2align 2
SYM(sword_ctx_switch):
    sub  sp, sp, #192
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x2, sp
    str  x2, [x0]

    mov  sp, x1
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #192
    ret

// A fresh task starts here rather than by returning into a caller: the link
// register in its prepared frame points at this, and it calls the entry point
// with a null frame pointer so a stack walk stops cleanly at the boundary.
.globl SYM(sword_ctx_start)
.p2align 2
SYM(sword_ctx_start):
    mov  x29, #0
    mov  x30, #0
    b    SYM(sword_fiber_entry)

#elif defined(__x86_64__)

// rdi = save_sp, rsi = resume_sp. SysV keeps rbx, rbp and r12-r15.
.globl SYM(sword_ctx_switch)
.p2align 4
SYM(sword_ctx_switch):
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)

    movq  %rsi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret

.globl SYM(sword_ctx_start)
.p2align 4
SYM(sword_ctx_start):
    xorq %rbp, %rbp
    jmp  SYM(sword_fiber_entry)

#else
#error "sword: no context switch for this architecture"
#endif

#if defined(__linux__)
// Nothing here needs an executable stack.
.section .note.GNU-stack,"",%progbits
#endif
