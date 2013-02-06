/*
 * This file is part of ltrace.
 * Copyright (C) 2012, 2013 Petr Machata, Red Hat Inc.
 * Copyright (C) 1998,2004,2008,2009 Juan Cespedes
 * Copyright (C) 2006 Ian Wienand
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <asm/ptrace.h>

#include "proc.h"
#include "common.h"
#include "output.h"
#include "ptrace.h"

#if (!defined(PTRACE_PEEKUSER) && defined(PTRACE_PEEKUSR))
# define PTRACE_PEEKUSER PTRACE_PEEKUSR
#endif

#if (!defined(PTRACE_POKEUSER) && defined(PTRACE_POKEUSR))
# define PTRACE_POKEUSER PTRACE_POKEUSR
#endif

#define off_r0 ((void *)0)
#define off_r7 ((void *)28)
#define off_ip ((void *)48)
#define off_pc ((void *)60)

void
get_arch_dep(struct process *proc)
{
	proc_archdep *a;

	if (!proc->arch_ptr)
		proc->arch_ptr = (void *)malloc(sizeof(proc_archdep));
	a = (proc_archdep *) (proc->arch_ptr);
	a->valid = (ptrace(PTRACE_GETREGS, proc->pid, 0, &a->regs) >= 0);
}

/* Returns 0 if not a syscall,
 *         1 if syscall entry, 2 if syscall exit,
 *         3 if arch-specific syscall entry, 4 if arch-specific syscall exit,
 *         -1 on error.
 */
int
syscall_p(struct process *proc, int status, int *sysnum)
{
	if (WIFSTOPPED(status)
	    && WSTOPSIG(status) == (SIGTRAP | proc->tracesysgood)) {
		/* get the user's pc (plus 8) */
		unsigned pc = ptrace(PTRACE_PEEKUSER, proc->pid, off_pc, 0);
		pc = pc - 4;
		/* fetch the SWI instruction */
		unsigned insn = ptrace(PTRACE_PEEKTEXT, proc->pid,
				       (void *)pc, 0);
		int ip = ptrace(PTRACE_PEEKUSER, proc->pid, off_ip, 0);

		if (insn == 0xef000000 || insn == 0x0f000000
		    || (insn & 0xffff0000) == 0xdf000000) {
			/* EABI syscall */
			*sysnum = ptrace(PTRACE_PEEKUSER, proc->pid, off_r7, 0);
		} else if ((insn & 0xfff00000) == 0xef900000) {
			/* old ABI syscall */
			*sysnum = insn & 0xfffff;
		} else {
			/* TODO: handle swi<cond> variations */
			/* one possible reason for getting in here is that we
			 * are coming from a signal handler, so the current
			 * PC does not point to the instruction just after the
			 * "swi" one. */
			output_line(proc, "unexpected instruction 0x%x at %p",
				    insn, pc);
			return 0;
		}
		if ((*sysnum & 0xf0000) == 0xf0000) {
			/* arch-specific syscall */
			*sysnum &= ~0xf0000;
			return ip ? 4 : 3;
		}
		/* ARM syscall convention: on syscall entry, ip is zero;
		 * on syscall exit, ip is non-zero */
		return ip ? 2 : 1;
	}
	return 0;
}

long
gimme_arg(enum tof type, struct process *proc, int arg_num,
	  struct arg_type_info *info)
{
	proc_archdep *a = (proc_archdep *) proc->arch_ptr;

	if (arg_num == -1) {	/* return value */
		return ptrace(PTRACE_PEEKUSER, proc->pid, off_r0, 0);
	}

	/* deal with the ARM calling conventions */
	if (type == LT_TOF_FUNCTION || type == LT_TOF_FUNCTIONR) {
		if (arg_num < 4) {
			if (a->valid && type == LT_TOF_FUNCTION)
				return a->regs.uregs[arg_num];
			if (a->valid && type == LT_TOF_FUNCTIONR)
				return a->func_arg[arg_num];
			return ptrace(PTRACE_PEEKUSER, proc->pid,
				      (void *)(4 * arg_num), 0);
		} else {
			return ptrace(PTRACE_PEEKDATA, proc->pid,
				      proc->stack_pointer + 4 * (arg_num - 4),
				      0);
		}
	} else if (type == LT_TOF_SYSCALL || type == LT_TOF_SYSCALLR) {
		if (arg_num < 5) {
			if (a->valid && type == LT_TOF_SYSCALL)
				return a->regs.uregs[arg_num];
			if (a->valid && type == LT_TOF_SYSCALLR)
				return a->sysc_arg[arg_num];
			return ptrace(PTRACE_PEEKUSER, proc->pid,
				      (void *)(4 * arg_num), 0);
		} else {
			return ptrace(PTRACE_PEEKDATA, proc->pid,
				      proc->stack_pointer + 4 * (arg_num - 5),
				      0);
		}
	} else {
		fprintf(stderr, "gimme_arg called with wrong arguments\n");
		exit(1);
	}

	return 0;
}

static arch_addr_t
arm_branch_dest(const arch_addr_t pc, const uint32_t insn)
{
	/* Bits 0-23 are signed immediate value.  */
	return pc + ((((insn & 0xffffff) ^ 0x800000) - 0x800000) << 2) + 8;
}

static int
get_next_pcs(struct process *proc,
	     const arch_addr_t pc, arch_addr_t next_pcs[2])
{
	uint32_t insn;
	if (proc_read_32(proc, pc, &insn) < 0)
		return -1;

	/* In theory, we sometimes don't even need to add any
	 * breakpoints at all.  If the conditional bits of the
	 * instruction indicate that it should not be taken, then we
	 * can just skip it altogether without bothering.  We could
	 * also emulate the instruction under the breakpoint.  GDB
	 * does both.
	 *
	 * Here, we make it as simple as possible (though We Accept
	 * Patches).  */
	int nr = 0;

	/* ARM can branch either relatively by using a branch
	 * instruction, or absolutely, by doing arbitrary arithmetic
	 * with PC as the destination.  XXX implement the latter.  */
	const int is_branch = ((insn >> 24) & 0x0e) == 0x0a;
	const int is_always = ((insn >> 24) & 0xf0) == 0xe0;
	if (is_branch) {
		next_pcs[nr++] = arm_branch_dest(pc, insn);
		if (is_always)
			return 0;
	}

	/* Otherwise take the next instruction.  */
	next_pcs[nr++] = pc + 4;
	return 0;
}

enum sw_singlestep_status
arch_sw_singlestep(struct process *proc, struct breakpoint *sbp,
		   int (*add_cb)(arch_addr_t, struct sw_singlestep_data *),
		   struct sw_singlestep_data *add_cb_data)
{
	arch_addr_t pc = get_instruction_pointer(proc);
	arch_addr_t next_pcs[2] = {};
	if (get_next_pcs(proc, pc, next_pcs) < 0)
		return SWS_FAIL;

	int i;
	for (i = 0; i < 2; ++i) {
		if (next_pcs[i] != 0 && add_cb(next_pcs[i], add_cb_data) < 0)
			return SWS_FAIL;
	}

	debug(1, "PTRACE_CONT");
	ptrace(PTRACE_CONT, proc->pid, 0, 0);
	return SWS_OK;
}
