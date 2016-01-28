/*
 * trap-syscalls.c
 *
 * This is the mechanism that reads the code to be run, and replace
 * system call instructions with traps to gain control of the execution
 * flow.
 */

/* Basic idea: we are a preloaded library whose constructor
 * - write-protects all executable pages
 *     -- using /proc/self/maps to enumerate them?
 *	YES, but must read using raw syscalls.
 *
 * - makes them writable, breakpoint any syscall instrs
 * - ... and then makes them unwritable again
 *
 * PROBLEM: vdso and vsyscall pages probably can't be write-protected
 * -- can we just override them? HMM.
 *
 */

#define _GNU_SOURCE
/* Don't use C library calls from this code! We run before the
 * C library is initialised. Also, the definitions in asm/ conflict
 * with some libc headers, particularly typedefs related to signal
 * handling. We use inline assembly to make the few system calls
 * that we need. */
#include <unistd.h>
#include <asm/signal.h>
#include <asm/fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "raw-syscalls.h"
#include "do-syscall.h"
#include "elfutil.h"

/* For clients building a standalone executable and wanting a simple test case, 
 * we allow them to set a test trap. */
void *__libsystrap_ignore_ud2_addr;

extern int etext;

static unsigned long read_hex_num(const char **p_c, const char *end)
{
	unsigned long cur = 0;
	while ((*p_c != end && (**p_c >= '0' && **p_c <= '9'))
			|| (**p_c >= 'a' && **p_c <= 'f'))
	{
		cur <<= 4;
		cur += ((**p_c >= '0' && **p_c <= '9') ? **p_c - '0'
				 : 10 + **p_c - 'a');
		++(*p_c);
	}
	return cur;
}

static const void *our_text_begin_address;
static const void *our_text_end_address;

static void replace_syscall(unsigned char *pos, unsigned len)
{
	debug_printf(1, "Replacing syscall at %p with trap\n", pos);
	
	assert(len >= 2);
	unsigned char *end = pos + len;
	while (pos != end)
	{
		switch (end - pos)
		{
			/* ud2 is 0x0f 0x0b */
			case 2: *pos++ = 0x0f; break;
			case 1: *pos++ = 0x0b; break;
			case 0: assert(0);
			default: *pos++ = 0x90; /* nop */ break;
		}
	}
}

static void walk_instructions(unsigned char *pos, unsigned char *end,
	void (*cb)(unsigned char *pos, unsigned len, void *arg), void *arg)
{
	unsigned char *cur = pos;
	while (cur < end)
	{
		/* FIXME: if our mapping includes some non-instructions, 
		 * and these accidentally decode into multi-byte instructions,
		 * we might get misaligned here. We *will* catch this when
		 * we do the paranoid second scan, but it would be better not
		 * to rely on this. */
		unsigned len = instr_len(cur, end);
		cb(cur, len, arg);
		cur += (len ? len : 1);
	}
}

static void instruction_cb(unsigned char *pos, unsigned len, void *arg)
{
	if (is_syscall_instr(pos, pos + len)) replace_syscall(pos, len);
}

static void saw_mapping(const char *line_begin_pos, const char *line_end_pos)
{
	const char *line_pos = line_begin_pos;
	unsigned long begin_addr = read_hex_num(&line_pos, line_end_pos);
	++line_pos;
	unsigned long end_addr = read_hex_num(&line_pos, line_end_pos);
	++line_pos;
	char r = *line_pos++;
	char w = *line_pos++;
	char x = *line_pos++;
	char p __attribute__((unused)) = *line_pos++;
	const char *newline = strchr(line_pos, '\n');
	if (!newline) newline = line_pos;
	const char *slash = strchr(line_pos, '/');
	const char *filename_src = NULL;
	size_t filename_sz;
	if (slash && slash < newline && *(slash - 1) == ' ')
	{
		filename_src = slash;
		const char *filename_end = strchr(filename_src, '\n');
		if (!filename_end)
		{
			// hmm -- slash but no newline
			filename_end = line_end_pos;
		}
		filename_sz = filename_end - filename_src;
	}
	else
	{
		// no filename present
		filename_sz = 0;
		filename_src = line_pos;
	}
	
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
	char filename[PATH_MAX + 1];
	size_t copy_sz = (filename_sz < PATH_MAX) ? filename_sz : PATH_MAX;
	strncpy(filename, filename_src, copy_sz);
	filename[copy_sz] = '\0';

	/* Skip ourselves, but remember our load address. */
	void *expected_mapping_end = (void*) page_boundary_up((uintptr_t) &etext);
	if ((const unsigned char *) end_addr >= (const unsigned char *) expected_mapping_end
		 && (const unsigned char *) begin_addr < (const unsigned char *) expected_mapping_end)
	{
		our_text_begin_address = (const void *) begin_addr;
		our_text_end_address = (const void *) end_addr;
		
		/* Compute our load address from the phdr p_vaddr of this segment.
		 * But how do we get at our phdrs?
		 * In general I think we need to hack the linker script to define a new symbol.
		 * But for now, just use the fact that it's very likely to be the lowest text addr. */
		our_load_address = (uintptr_t) our_text_begin_address;

		debug_printf(1, "Skipping our own text mapping: %p-%p\n", 
			(void*) begin_addr, (void*) end_addr);
		
		return;
	}

	if (x == 'x')
	{
		if (w != 'w')
		{
			int ret = raw_mprotect((const void *) begin_addr,
				(const char *) end_addr - (const char *) begin_addr,
				PROT_READ | PROT_WRITE | PROT_EXEC);
			
			/* If we failed, it might be on the vdso page. */
			assert(ret == 0 || (intptr_t) begin_addr < 0);
			if (ret != 0 && (intptr_t) begin_addr < 0)
			{
				/* vdso/vsyscall handling: since we can't rewrite the instructions on these 
				 * pages, instead we should execute-protect them. Then, when we take a trap, 
				 * we need to emulate the instructions there. FIXME: implement this. */
				
				debug_printf(1, "Couldn't rewrite nor protect vdso mapping at %p\n", 
						(void*) begin_addr);
				return;
			}
		}

		// it's executable; scan for syscall instructions
		unsigned char *begin_instr_pos;
		/* An executable mapping might include some non-instructions 
		 * that will cause our instruction walker to get misaligned. 
		 * Instead, we would like to walk the *sections* individually,
		 * then re-traverse the whole thing. So we mmap the section
		 * header table. PROBLEM: we can't re-open a file that is
		 * guaranteed to be the same. */
		void *base_addr = NULL;
		const void *next_section_start = vaddr_to_next_instruction_start(
			(unsigned char *) begin_addr, filename[0] ? filename : NULL, &base_addr);

		if (next_section_start)
		{
			begin_instr_pos = (unsigned char *) next_section_start;
		} else begin_instr_pos = (unsigned char *) begin_addr;
		
		unsigned char *end_instr_pos = (unsigned char *) end_addr;
		/* What to do about byte sequences that look like syscalls 
		 * but are "in the middle" of instructions? 
		 * How do we know where to *start* parsing an instruction stream? 
		 * 
		 * For now, we
		 * - start parsing at the beginning only
		 * - do fixups
		 * - then do another pass where we detect remaining syscall-instruction-alikes
		 * - ... and warn if we see any
		 * 
		 * What about ud2-alikes that don't correspond to replaced instructions?
		 * No problem: we just need to remember which sites we replaced.
		 * If we hit a ud2 that's not at such a site, we just do ud2.
		 * FIXME: implement this.
		 */
		char debug_buf[line_end_pos - line_begin_pos + 1];
		strncpy(debug_buf, line_begin_pos, line_end_pos - line_begin_pos);
		debug_buf[sizeof debug_buf - 1] = '\0';
		// assert that line_end_pos 
		debug_printf(1, "Scanning for syscall instructions within %p-%p (%s)\n",
			(void*) begin_addr, (void*) end_addr, debug_buf);
		
		walk_instructions(begin_instr_pos, end_instr_pos, instruction_cb, NULL);
		/* Now the paranoid second scan: check for in-betweens. */
		unsigned char *instr_pos = (unsigned char *) begin_addr;
		while (instr_pos != end_instr_pos)
		{
			if (is_syscall_instr(instr_pos, end_instr_pos))
			{
				debug_printf(0, "Warning: after instrumentation, bytes at %p "
					"could make a syscall on violation of control flow integrity\n", 
					instr_pos);
			}
			++instr_pos;
		}

		// restore original perms
		if (w != 'w')
		{
			int ret = raw_mprotect((const void *) begin_addr,
				(const char *) end_addr - (const char *) begin_addr,
				(r == 'r' ? PROT_READ : 0)
			|   (w == 'w' ? PROT_WRITE : 0)
			|   (x == 'x' ? PROT_EXEC : 0));
			assert(ret == 0);
		}
	}
}

static void handle_sigill(int num);

int debug_level __attribute__((visibility("hidden")));
int sleep_for_seconds __attribute__((visibility("hidden")));
int stop_self __attribute__((visibility("hidden")));
int self_pid __attribute__((visibility("hidden")));
void **p_err_stream __attribute__((visibility("hidden"))) = &stderr;

/* We initialize our error-reporting stuff, but don't actually 
 * set up any traps. That's left to the client. */
static void __attribute__((constructor)) startup(void)
{
	char *debug_level_str = getenv("TRAP_SYSCALLS_DEBUG");
	char *sleep_for_seconds_str = getenv("TRAP_SYSCALLS_SLEEP_FOR_SECONDS");
	char *stop_self_str = getenv("TRAP_SYSCALLS_STOP_SELF");
	stop_self = (stop_self_str != NULL);
	struct timespec one_second = { /* seconds */ 1, /* nanoseconds */ 0 };
	if (debug_level_str) debug_level = atoi(debug_level_str);
	if (sleep_for_seconds_str) sleep_for_seconds = atoi(sleep_for_seconds_str);
	debug_printf(0, "Debug level is %s=%d.\n", debug_level_str, debug_level);
	if (stop_self) {
		self_pid = raw_getpid();
		debug_printf(0, "TRAP_SYSCALLS_STOP_SELF is set, sending SIGSTOP to self (pid %d)\n", self_pid);
		raw_kill(self_pid, SIGSTOP);
	}
	debug_printf(0, "TRAP_SYSCALLS_SLEEP_FOR_SECONDS is %s, pausing for %d seconds", sleep_for_seconds_str, sleep_for_seconds);
	for (int i = 0; i < sleep_for_seconds; i++) {
		raw_nanosleep(&one_second, NULL);
		debug_printf(0, ".");
	}
	debug_printf(0, "\n");

	//trap_all_mappings();
	// install_sigill_handler();
}

void trap_all_mappings(void)
{
	int fd = raw_open("/proc/self/maps", O_RDONLY);

	if (fd != -1)
	{
		// we use a simple buffer and a read loop
		char buf[8192];
		unsigned int ret;
		char *buf_pos = &buf[0]; // the next position to fill in the buffer
		char *entry_start_pos = &buf[0]; // the position
		size_t size_requested;
		do
		{
			// read some stuff, perhaps filling up the buffer
			size_requested = sizeof buf - (buf_pos - buf);
			ret = raw_read(fd, buf_pos, size_requested);
			char *buf_limit = buf_pos + ret;
			assert(buf_limit <= &buf[sizeof buf]);

			// we have zero or more complete entries in the buffer; iterate over them
			char *seek_pos;
			while (1)
			{
				seek_pos = entry_start_pos;
				// search forward for a newline
				while (seek_pos != buf_limit && *seek_pos != '\n')
				{ ++seek_pos; }

				// did we find one?
				if (seek_pos == buf_limit)
				{
					// no!
					// but we have a partial entry in the buffer
					// between entry_start_pos and seek_pos;
					// copy it to the start, re-set and continue
					__builtin_memmove(&buf[0], entry_start_pos, seek_pos - entry_start_pos);
					buf_pos = &buf[seek_pos - entry_start_pos];
					entry_start_pos = &buf[0];
					break;
				}
				else
				{
					assert(*seek_pos == '\n');
					// we have a complete entry; read it and advance entry_start_pos
					char debug_buf1[seek_pos - entry_start_pos + 1];
					strncpy(debug_buf1, entry_start_pos, seek_pos - entry_start_pos);
					debug_buf1[sizeof debug_buf1 - 1] = '\0';
					debug_printf(1, "DEBUG: entry is: %s\n", debug_buf1);
					char debug_buf2[buf_pos - buf];
					strncpy(debug_buf2, buf, buf_pos - buf);
					debug_buf2[sizeof debug_buf2 - 1] = '\0';
					debug_printf(1, "DEBUG: buffer is: %s", debug_buf2);
					saw_mapping(entry_start_pos, seek_pos);
					entry_start_pos = seek_pos + 1;
					// if the newline was the last in the buffer, break and read more
					if (entry_start_pos == buf_pos + sizeof buf)
					{ buf_pos = entry_start_pos = &buf[0]; break; }

					// else we might have another entry; go round again
					continue;
				}
			}
		} while (ret > 0);
		raw_close(fd);
	}
}

void install_sigill_handler(void)
{
	/* Install our SIGILL (was SIGTRAP, but that interferes with gdb) handler.
	 * Linux seems to require us to provide a restorer; the code is in restore_rt. */
	struct sigaction action = {
		//.sa_sigaction = &handle_sigtrap,
		.sa_handler = &handle_sigill,
		.sa_mask = 0,
		.sa_flags = /*SA_SIGINFO |*/ 0x04000000u /* SA_RESTORER */ | /*SA_RESTART |*/ SA_NODEFER,
		.sa_restorer = restore_rt
	};
	struct sigaction oldaction;
	raw_rt_sigaction(SIGILL, &action, &oldaction);

	/* Un-executablize our own code, except for the signal handler and the remainder of
	 * this function and those afterwards.
	 *
	 * For this, we need our load address. How can we get this? We've already seen it! */
	// long int len = &&exit_and_return - our_text_begin_address;
	// long int ret;
	// long int longprot = PROT_NONE;
	// long int op = SYS_mprotect;

	//	__asm__ (".align 4096");
exit_and_return:
	//__asm__ volatile ("movq %0, %%rdi      # \n\
	//		   movq %1, %%rsi      # \n\
	//		   movq %2, %%rdx      # \n\
	//		  "FIX_STACK_ALIGNMENT " \n\
	//		   movq %3, %%rax      # \n\
	//		   syscall	     # do the syscall \n\
	//		  "UNFIX_STACK_ALIGNMENT " \n"
	//  : /* no output*/ : "rm"(our_text_begin_address), "rm"(len), "rm"(longprot), "rm"(op) :  "%rax", "r12", SYSCALL_CLOBBER_LIST);
	return;
}

// For debug printing inside handle_sigill we have to know
// that it's our own debug printing in order to filter it
// out of the footprints, hence this noinline function
// rather than using the normal macro
__attribute__ ((noinline)) static void _handle_sigill_debug_printf(int level, const char *fmt, ...) {
	 va_list vl;
	 va_start(vl, fmt);
	 if ((level) <= debug_level) {
		  vfprintf(*p_err_stream, fmt, vl);
		  fflush(*p_err_stream);
	 }
	 va_end(vl);
}

static void handle_sigill(int n)
{
	unsigned long *frame_base = __builtin_frame_address(0);
	struct ibcs_sigframe *p_frame = (struct ibcs_sigframe *) (frame_base + 1);

	/* Decode the syscall using sigcontext. */
	_handle_sigill_debug_printf(1, "Took a trap from instruction at %p", p_frame->uc.uc_mcontext.rip);
#ifdef EXECUTABLE
	if (p_frame->uc.uc_mcontext.rip == (uintptr_t) ignore_ud2_addr)
	{
		_handle_sigill_debug_printf(1, " which is our test trap address; continuing.\n");
		resume_from_sigframe(0, p_frame, 2);
		return;
	}
#endif
	unsigned long syscall_num = (unsigned long) p_frame->uc.uc_mcontext.rax;
	assert(syscall_num >= 0);
	assert(syscall_num < SYSCALL_MAX);
	_handle_sigill_debug_printf(1, " which we think is syscall %s/%d\n",
		syscall_names[syscall_num], syscall_num);

	/* FIXME: check whether this syscall creates executable mappings; if so,
	 * we make them nx, do the rewrite, then make them x. */

	struct generic_syscall gsp = {
		.saved_context = p_frame,
		.syscall_number = syscall_num,
		.args = {
			p_frame->uc.uc_mcontext.rdi,
			p_frame->uc.uc_mcontext.rsi,
			p_frame->uc.uc_mcontext.rdx,
			p_frame->uc.uc_mcontext.r10,
			p_frame->uc.uc_mcontext.r8,
			p_frame->uc.uc_mcontext.r9
		}
	};

	do_syscall_and_resume(&gsp); // inline
}
