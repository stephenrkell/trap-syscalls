/*
 * do-syscall.c
 *
 * This file contains the do_syscall() function, which performs the
 * action expected from the trapped system call.
 */

#include "do-syscall.h"
#include "syscall-handlers.h"
#include "raw-syscalls.h"

long int do_syscall (struct syscall *sys)
{
        struct generic_syscall *gsp = (struct generic_syscall *) sys;

#ifdef DUMP_SYSCALLS
        write_string("Performing syscall with opcode: ");
	raw_write(2, fmt_hex_num(gsp->syscall_number), 18);
	write_string("\n");
#endif

        long int ret = syscalls[sys->syscall_number](gsp);

#ifdef DUMP_SYSCALLS
        write_string("Syscall returned value: ");
        raw_write(2, fmt_hex_num(ret), 18);
        write_string("\n");
#endif

        return ret;
}