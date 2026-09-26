/*
 * switch.S - ctx_switch(struct pcb *old, struct pcb *new_pcb).
 *
 * Saves r4-r11, SP, LR into *old, then restores them from *new_pcb and
 * resumes it with `bx lr`. r0-r3/r12 are caller-saved per AAPCS and are
 * intentionally not preserved across a switch (like setjmp/longjmp and
 * Mach's switch_context, the switcher never resumes in its own frame).
 *
 * Offsets must match struct pcb in pcb.h:
 *   r4-r11 at 0..28, sp at 32, lr at 36.
 */
    .syntax unified
    .arm
    .text
    .global ctx_switch
    .type ctx_switch, %function
ctx_switch:
    /* r0 = old, r1 = new. Save callee-saved state into old. */
    stmia r0, {r4-r11}       /* offsets 0..28 */
    str sp, [r0, #32]
    str lr, [r0, #36]

    /* Restore new. r1 stays valid across the SP switch (it's a register). */
    ldmia r1, {r4-r11}
    ldr sp, [r1, #32]
    ldr lr, [r1, #36]
    bx lr
    .size ctx_switch, . - ctx_switch
    .end
