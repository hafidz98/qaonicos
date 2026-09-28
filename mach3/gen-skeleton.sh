#!/bin/bash
# Generator stub kerangka MD ARM untuk port Mach 3 (M1).
# Menulis mach3/kernel/arm/* dan mach3/kernel/mach/arm/*.
set -e
R=~/workspace/qaonic_os/mach3
A=$R/kernel/arm
M=$R/kernel/mach/arm
mkdir -p $A/mp $M

hdr() { # $1=relpath $2=desc $3=adapt
cat > "$A/$1" <<EOF
/*
 * mach3/kernel/arm/$1 -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include kernel MI via <machine/$1>.
 * Dimodelkan dari Prajna/mach kernel/mips/$(basename $1).
 *
 * WAJIB disediakan:
 * $2
 *
 * Adaptasi dari: $3
 */
EOF
}

mhdr() { # $1=relpath $2=desc
cat > "$M/$1" <<EOF
/*
 * mach3/kernel/mach/arm/$1 -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include via <mach/machine/$1>.
 * Dimodelkan dari Prajna/mach kernel/mach/mips/$1.
 *
 * WAJIB disediakan:
 * $2
 */
EOF
}

src() { # $1=relpath $2=desc $3=adapt
cat > "$A/$1" <<EOF
/*
 * mach3/kernel/arm/$1 -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * $2
 *
 * Adaptasi dari: $3
 */
EOF
}

# --- machine/ headers ---
hdr asm.h "- Makro assembler: ENTRY/LEAF/END, eksport simbol ke C." "rv1103-bringup/vectors.S, start.S (konvensi label)"
hdr asm_linkage.h "- Makro linkage C<->asm (nama simbol, alignment)." "rv1103-bringup/aeabi.S"
hdr ast.h "- Definisi AST (asynchronous system trap) khusus ARM." "mips/ast.h sebagai pola; isi dari nol"
hdr ast_types.h "- Tipe ast_t dan konstanta AST." "mips/ast.h sebagai pola"
hdr cpu.h "- Identifikasi CPU (cortex-a7), cpu_info." "rv1103-bringup/board.h"
hdr cpu_number.h "- cpu_number() untuk uniprocessor (return 0)." "sebaris, dari nol"
hdr db_machdep.h "- Hook kernel debugger (ddb): db_machine_init dkk." "opsional M2; stub dulu"
hdr db_trace.h "- Stack trace khusus ARM (unwind frame)." "opsional M2; stub dulu"
hdr kttd_machdep.h "- Hook KTTD tracing khusus mesin." "opsional; stub dulu"
hdr lock.h "- Implementasi simple_lock ARM (ldrex/strex atau irq-mask unipro)." "rv1103-bringup/irq.c (pola mask IRQ)"
hdr mach_param.h "- HZ (clock tick), parameter mesin lain." "rv1103-bringup/timer.c (tick rate)"
hdr machine_routines.h "- Deklarasi rutin MD: bcopy, copyin, copyout, splx, dll." "mips sebagai pola"
hdr machspl.h "- Level SPL + makro splhigh/splsched/spl0 untuk ARM (basis CPSR I-bit)." "rv1103-bringup/irq.c"
hdr pmap.h "- API pmap: pmap_t, kernel_pmap/active_pmap, pmap_pte(), pmap_protect/enter/remove, statistik." "rv1103-bringup/pmap.h (struktur sudah mirip!)"
hdr pte.h "- Definisi bit PTE ARMv7 short-descriptor (AP, TEX, C/B, XN, nG)." "rv1103-bringup/pmap.c + armv7/"
hdr regdef.h "- Nama register untuk file .s." "sebaris, dari nol"
hdr sched_param.h "- Parameter scheduler khusus mesin." "mips sebagai pola"
hdr setjmp.h "- jmp_buf ARM + setjmp/longjmp." "dari nol (atau pola mips)"
hdr thread.h "- Status thread MD: struct arm_pcb, thread_state." "rv1103-bringup/pcb.h"
hdr time_stamp.h "- Timestamp resolusi tinggi (CNTVCT)." "rv1103-bringup/timer.c"
hdr timer.h "- Interface timer MD (set_timer, timer_intr)." "rv1103-bringup/timer.c"
hdr vm_tuning.h "- Tuning VM (boleh kosong seperti mips). Tidak wajib diisi." "-"
hdr xpr.h "- Buffer trace XPR khusus mesin." "opsional; stub dulu"
hdr mp/mp.h "- Stub multiprosesor (NCPUS=1, uniprocessor)." "sebaris"

# --- mach/machine/ headers ---
mhdr boolean.h "- boolean_t untuk ARM (int)."
mhdr exception.h "- Kode exception khusus ARM."
mhdr kern_return.h "- kern_return_t untuk ARM (int)."
mhdr machine_types.defs "- Definisi tipe MIG khusus mesin."
mhdr syscall_sw.h "- Tabel syscall software (trap) ARM."
mhdr thread_status.h "- Flavor thread state: ARM_THREAD_STATE, ARM_VFP_STATE."
mhdr vm_param.h "- VM_MIN_ADDRESS, VM_MAX_ADDRESS, PAGE_SIZE=4096, BYTE_SIZE=8."
mhdr vm_types.h "- vm_offset_t/vm_size_t (natural_t 32-bit)."

# --- sumber MD ---
src locore.s "- Vektor exception ARMv7 (reset/undef/svc/pabt/dabt/irq/fiq), _start, trampoline ke C." "rv1103-bringup/vectors.S + start.S"
src trap.c "- Dispatch trap: trap(), syscall_entry, page_fault handler -> vm_fault." "rv1103-bringup/trap.c"
src pmap.c "- Implementasi pmap ARMv7: pmap_bootstrap, pmap_create/destroy, pmap_enter/remove/protect, pmap_pageable." "rv1103-bringup/pmap.c (inti sudah ada!)"
src pcb.c "- PCB: pcb_init, switch stacks, save/restore konteks." "rv1103-bringup/pcb.c + switch.S"
src context.s "- switch_context() assembler murni." "rv1103-bringup/switch.S"
src arm_init.c "- machine_startup(): parse boot info, pmap_bootstrap, cpu_startup, buka console." "rv1103-bringup/main.c + kernel_main.c (alur boot)"
src clock.c "- System clock dari ARM generic timer (CNTVCT), hardclock()." "rv1103-bringup/timer.c"
src machdep.c "- Rutin misc MD: cpu_sleep, halts, panic MD." "rv1103-bringup/lib.c"
src copy.s "- bcopy/bzero/copyin/copyout assembler (atau C)." "rv1103-bringup/lib.c (sudah ada bcopy/bzero)"
src fpu.c "- Save/restore VFPv4 per thread (fpu_save/fpu_restore)." "rv1103-bringup/fpu.c (sudah ada, 1:1)"
src gic.c "- Driver GIC-400: init, enable/disable IRQ, EOI." "rv1103-bringup/gic.c + irq.c (sudah ada, 1:1)"
src uart.c "- Console: cnputc/cngetc via PL011 (QEMU virt) / DW (RV1103)." "rv1103-bringup/uart.c (sudah ada, 1:1)"

echo "OK: $(find $R/kernel -type f | wc -l) file kerangka"
