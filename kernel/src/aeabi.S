/* aeabi.S - AAPCS helper rutin yang biasanya disediakan libgcc/compiler-rt.
 *
 * Clang bare-metal (--target=arm-none-eabi) di sini tidak punya
 * compiler-rt builtins untuk ARM, jadi __aeabi_uldivmod diimplementasikan
 * manual: long division 64-bit shift-subtract, murni instruksi integer
 * (tidak boleh memakai pembagian 64-bit di dalamnya: itu rekursi).
 *
 * AAPCS: masuk r0,r1 = numerator (64-bit), r2,r3 = denominator (64-bit);
 *        keluar r0,r1 = quotient, r2,r3 = remainder.
 */
    .syntax unified
    .text
    .arm

    .global __aeabi_uldivmod
    .type   __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4-r8, lr}
    mov     r4, #0              @ rem_lo
    mov     r5, #0              @ rem_hi
    mov     r6, #0              @ quot_lo
    mov     r7, #0              @ quot_hi
    mov     r8, #64             @ jumlah iterasi
1:
    @ quot <<= 1 (lsls mengeset C fresh dari bit31 r6; adc merambatkannya)
    lsls    r6, r6, #1
    adc     r7, r7, r7
    @ n <<= 1 ; bit teratas n masuk ke C
    lsls    r0, r0, #1
    adc     r1, r1, r1
    @ rem = rem*2 + C
    adcs    r4, r4, r4
    adc     r5, r5, r5
    @ bila rem >= d: rem -= d ; quot |= 1
    cmp     r5, r3
    bhi     2f
    blo     3f
    cmp     r4, r2
    blo     3f
2:
    subs    r4, r4, r2
    sbc     r5, r5, r3
    orr     r6, r6, #1
3:
    subs    r8, r8, #1
    bne     1b
    @ pindahkan hasil ke register keluaran AAPCS
    mov     r0, r6              @ quot_lo
    mov     r1, r7              @ quot_hi
    mov     r2, r4              @ rem_lo
    mov     r3, r5              @ rem_hi
    pop     {r4-r8, pc}
    .size   __aeabi_uldivmod, .-__aeabi_uldivmod
