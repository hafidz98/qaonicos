/*
 * mach3/mach3.ld -- linker script for Mach 3 ARM (QEMU -M virt).
 * Kernel linked at 0x40000000, identity-mapped.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x40000000;

	.text.boot : {
		KEEP(*(.text.boot))
	}

	.text : {
		*(.text*)
	}

	.rodata : {
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	_bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	_bss_end = .;
	_end = .;
	edata = .;
	end = .;
}
