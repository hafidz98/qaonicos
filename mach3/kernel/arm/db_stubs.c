/*
 * mach3/kernel/arm/db_stubs.c -- ddb (kernel debugger) MD stubs (M3).
 *
 * The MI ddb is compiled in.  Full ddb support (disassembler, register
 * windows, COFF symbols) is future work.  These stubs let it link;
 * entering ddb panics.
 */
#include <mach/machine/vm_types.h>
#include <mach/boolean.h>
#include <machine/thread.h>

extern void	panic(const char *, ...);

/* Register state for ddb (defined in db_machdep.h as extern). */
struct arm_saved_state	db_regs;
struct arm_saved_state	*db_eregs = 0;

/* Current exception frame for ddb. */
void	*db_cur_exc_frame = 0;

/* Task name for ddb 'show task' (M3: dummy). */
char	*db_task_name = "kernel";

/* COFF symbol table (not used with ELF; stubs). */
void
coff_db_sym_init(void)
{
}

int
coff_db_lookup(void *symtab, const char *name, void *val)
{
	return 0;
}

int
coff_db_search_symbol(void *symtab, vm_offset_t val, char *name,
		      void *off)
{
	return 0;
}

int
coff_db_line_at_pc(void *symtab, char *filename, unsigned int *linenum,
		   vm_offset_t pc)
{
	return 0;
}

int
coff_db_symbol_values(void *symtab, const char *name, char *filename,
		      void *val)
{
	return 0;
}

/* Instruction emulation for single-step (M3: not implemented). */
int
branch_taken(int inst, vm_offset_t pc, void *regs)
{
	panic("branch_taken: not implemented in M3");
	return 0;
}

int
inst_unconditional_flow_transfer(int inst)
{
	return 0;
}

unsigned int
getreg_val(void *regs, int regno)
{
	return 0;
}

/* Disassembler (M3: not implemented). */
void
db_disasm(vm_offset_t loc, boolean_t altfmt)
{
	panic("db_disasm: not implemented in M3");
}

/* Memory access for ddb (identity-mapped: direct). */
void
db_read_bytes(vm_offset_t addr, int size, char *data, void *task)
{
	int i;
	for (i = 0; i < size; i++)
		data[i] = ((char *)addr)[i];
}

void
db_write_bytes(vm_offset_t addr, int size, char *data, void *task)
{
	int i;
	for (i = 0; i < size; i++)
		((char *)addr)[i] = data[i];
}

int
db_phys_eq(vm_offset_t a, vm_offset_t b)
{
	/* M3: identity-mapped, physical == virtual. */
	return a == b;
}

void
db_stack_trace_cmd(void *addr, boolean_t have_addr, int count, char *modif)
{
	panic("db_stack_trace_cmd: not implemented in M3");
}

/* Access check for ddb (M3: allow kernel addresses). */
int
db_check_access(vm_offset_t addr, int size, int rw)
{
	/* Allow; ddb will panic on entry anyway. */
	return 1;
}

/* setjmp/longjmp for ddb (M3: minimal). */
int
_setjmp(void *jb)
{
	/* Not implemented; ddb panics on entry. */
	return 0;
}

void
_longjmp(void *jb, int val)
{
	panic("_longjmp: not implemented in M3");
}
