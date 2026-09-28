/*
 * mach3/kernel/arm/mig_stubs.c -- MIG client stubs (M3).
 *
 * The M2 MIG stub generator only emitted headers.  These are the
 * client-side RPCs a task uses to talk to its memory object (pager).
 * No pager exists in M3 boot; they panic if called.  Real MIG output
 * (or hand-marshalled IPC) is future work.
 */
#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/machine/vm_types.h>
#include <mach/vm_prot.h>

extern void	panic(const char *, ...);

#define	MIG_PANIC(name) \
	panic("MIG stub called: " name " (no pager in M3)")

/* --- memory_object_user (from mach/memory_object.defs) --- */
kern_return_t
memory_object_data_request(mach_port_t memory_object,
			   mach_port_t memory_control,
			   vm_offset_t offset, vm_size_t length,
			   vm_prot_t desired_access)
{
	MIG_PANIC("memory_object_data_request");
	return KERN_FAILURE;
}

kern_return_t
memory_object_data_return(mach_port_t memory_object,
			  mach_port_t memory_control,
			  vm_offset_t offset, vm_offset_t data,
			  unsigned int data_cnt, boolean_t dirty,
			  boolean_t kernel_copy)
{
	MIG_PANIC("memory_object_data_return");
	return KERN_FAILURE;
}

kern_return_t
memory_object_data_initialize(mach_port_t memory_object,
			      mach_port_t memory_control,
			      vm_offset_t offset, vm_offset_t data,
			      unsigned int data_cnt)
{
	MIG_PANIC("memory_object_data_initialize");
	return KERN_FAILURE;
}

kern_return_t
memory_object_data_unlock(mach_port_t memory_object,
			  mach_port_t memory_control,
			  vm_offset_t offset, vm_size_t length,
			  vm_prot_t desired_access)
{
	MIG_PANIC("memory_object_data_unlock");
	return KERN_FAILURE;
}

kern_return_t
memory_object_data_write(mach_port_t memory_object,
			 mach_port_t memory_control,
			 vm_offset_t offset, vm_offset_t data,
			 unsigned int data_cnt)
{
	MIG_PANIC("memory_object_data_write");
	return KERN_FAILURE;
}

kern_return_t
memory_object_lock_completed(mach_port_t memory_object,
			     mach_port_t memory_control,
			     vm_offset_t offset, vm_size_t length)
{
	MIG_PANIC("memory_object_lock_completed");
	return KERN_FAILURE;
}

kern_return_t
memory_object_supply_completed(mach_port_t memory_object,
			       mach_port_t memory_control,
			       vm_offset_t offset, vm_size_t length,
			       kern_return_t result,
			       vm_offset_t error_offset)
{
	MIG_PANIC("memory_object_supply_completed");
	return KERN_FAILURE;
}

kern_return_t
memory_object_change_completed(mach_port_t memory_object,
			       mach_port_t memory_control,
			       boolean_t may_cache, boolean_t copy_strategy)
{
	MIG_PANIC("memory_object_change_completed");
	return KERN_FAILURE;
}

kern_return_t
memory_object_terminate(mach_port_t memory_object,
			mach_port_t memory_control)
{
	MIG_PANIC("memory_object_terminate");
	return KERN_FAILURE;
}

kern_return_t
memory_object_copy(mach_port_t memory_object,
		   mach_port_t memory_control,
		   vm_offset_t offset, vm_size_t length,
		   mach_port_t new_memory_object)
{
	MIG_PANIC("memory_object_copy");
	return KERN_FAILURE;
}

/* --- MIG server dispatch (M3: servers not run; stubs panic) --- */
boolean_t
mach_server_routine(void *in, void *out)
{
	MIG_PANIC("mach_server_routine");
	return 0;
}

boolean_t
mach_port_server_routine(void *in, void *out)
{
	MIG_PANIC("mach_port_server_routine");
	return 0;
}

boolean_t
mach_host_server_routine(void *in, void *out)
{
	MIG_PANIC("mach_host_server_routine");
	return 0;
}

boolean_t
device_server_routine(void *in, void *out)
{
	MIG_PANIC("device_server_routine");
	return 0;
}

boolean_t
mach4_server_routine(void *in, void *out)
{
	MIG_PANIC("mach4_server_routine");
	return 0;
}

/* --- ds_device replies (device server client stubs) --- */
#include <device/device_reply.h>

kern_return_t
ds_device_open_reply(mach_port_t reply_port,
		     mach_msg_type_name_t reply_portPoly,
		     kern_return_t return_code,
		     mach_port_t device_port)
{
	MIG_PANIC("ds_device_open_reply");
	return KERN_FAILURE;
}

kern_return_t
ds_device_write_reply(mach_port_t reply_port,
		      mach_msg_type_name_t reply_portPoly,
		      kern_return_t return_code,
		      int bytes_written)
{
	MIG_PANIC("ds_device_write_reply");
	return KERN_FAILURE;
}

kern_return_t
ds_device_write_reply_inband(mach_port_t reply_port,
			     mach_msg_type_name_t reply_portPoly,
			     kern_return_t return_code,
			     int bytes_written)
{
	MIG_PANIC("ds_device_write_reply_inband");
	return KERN_FAILURE;
}

kern_return_t
ds_device_read_reply(mach_port_t reply_port,
		     mach_msg_type_name_t reply_portPoly,
		     kern_return_t return_code,
		     io_buf_ptr_t data,
		     mach_msg_type_number_t dataCnt)
{
	MIG_PANIC("ds_device_read_reply");
	return KERN_FAILURE;
}

kern_return_t
ds_device_read_reply_inband(mach_port_t reply_port,
			    mach_msg_type_name_t reply_portPoly,
			    kern_return_t return_code,
			    io_buf_ptr_inband_t data,
			    mach_msg_type_number_t dataCnt)
{
	MIG_PANIC("ds_device_read_reply_inband");
	return KERN_FAILURE;
}

/* --- r_memory_object_* (server-side replies used by dev_pager) --- */
kern_return_t
r_memory_object_data_error(mach_port_t pager_request,
			   mach_port_t pager_requestPoly,
			   vm_offset_t offset,
			   vm_size_t length,
			   kern_return_t error_value)
{
	MIG_PANIC("r_memory_object_data_error");
	return KERN_FAILURE;
}

kern_return_t
r_memory_object_data_provided(mach_port_t pager_request,
			      mach_port_t pager_requestPoly,
			      vm_offset_t offset,
			      vm_offset_t data,
			      unsigned int data_cnt,
			      vm_prot_t lock_value)
{
	MIG_PANIC("r_memory_object_data_provided");
	return KERN_FAILURE;
}

kern_return_t
r_memory_object_set_attributes(mach_port_t pager_request,
			       mach_port_t pager_requestPoly,
			       boolean_t cache,
			       boolean_t copy_strategy)
{
	MIG_PANIC("r_memory_object_set_attributes");
	return KERN_FAILURE;
}

/* --- memory_object_default (default pager) --- */
kern_return_t
memory_object_create(mach_port_t default_pager,
		     mach_port_t *new_memory_object,
		     vm_size_t object_size)
{
	MIG_PANIC("memory_object_create");
	return KERN_FAILURE;
}

kern_return_t
memory_object_init(mach_port_t default_pager,
		   mach_port_t memory_object,
		   mach_port_t memory_control,
		   vm_size_t object_size,
		   mach_port_t *new_memory_object)
{
	MIG_PANIC("memory_object_init");
	return KERN_FAILURE;
}
