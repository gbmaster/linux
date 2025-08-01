// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/process.c
 *
 * Original Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1996-2000 Russell King - Converted to ARM.
 * Copyright (C) 2012 ARM Ltd.
 */
#include <linux/compat.h>
#include <linux/efi.h>
#include <linux/elf.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/nospec.h>
#include <linux/stddef.h>
#include <linux/sysctl.h>
#include <linux/unistd.h>
#include <linux/user.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/elfcore.h>
#include <linux/pm.h>
#include <linux/tick.h>
#include <linux/utsname.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/hw_breakpoint.h>
#include <linux/personality.h>
#include <linux/notifier.h>
#include <trace/events/power.h>
#include <linux/percpu.h>
#include <linux/thread_info.h>
#include <linux/prctl.h>
#include <linux/stacktrace.h>

#include <asm/alternative.h>
#include <asm/arch_timer.h>
#include <asm/compat.h>
#include <asm/cpufeature.h>
#include <asm/cacheflush.h>
#include <asm/exec.h>
#include <asm/fpsimd.h>
#include <asm/gcs.h>
#include <asm/mmu_context.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/pointer_auth.h>
#include <asm/stacktrace.h>
#include <asm/switch_to.h>
#include <asm/system_misc.h>

#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

/*
 * Function pointers to optional machine specific functions
 */
void (*pm_power_off)(void);
EXPORT_SYMBOL_GPL(pm_power_off);

#ifdef CONFIG_HOTPLUG_CPU
void __noreturn arch_cpu_idle_dead(void)
{
       cpu_die();
}
#endif

/*
 * Called by kexec, immediately prior to machine_kexec().
 *
 * This must completely disable all secondary CPUs; simply causing those CPUs
 * to execute e.g. a RAM-based pin loop is not sufficient. This allows the
 * kexec'd kernel to use any and all RAM as it sees fit, without having to
 * avoid any code or data used by any SW CPU pin loop. The CPU hotplug
 * functionality embodied in smpt_shutdown_nonboot_cpus() to achieve this.
 */
void machine_shutdown(void)
{
	smp_shutdown_nonboot_cpus(reboot_cpu);
}

/*
 * Halting simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this.
 */
void machine_halt(void)
{
	local_irq_disable();
	smp_send_stop();
	while (1);
}

/*
 * Power-off simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this. When the system power is turned off, it will take all CPUs
 * with it.
 */
void machine_power_off(void)
{
	local_irq_disable();
	smp_send_stop();
	do_kernel_power_off();
}

/*
 * Restart requires that the secondary CPUs stop performing any activity
 * while the primary CPU resets the system. Systems with multiple CPUs must
 * provide a HW restart implementation, to ensure that all CPUs reset at once.
 * This is required so that any code running after reset on the primary CPU
 * doesn't have to co-ordinate with other CPUs to ensure they aren't still
 * executing pre-reset code, and using RAM that the primary CPU's code wishes
 * to use. Implementing such co-ordination would be essentially impossible.
 */
void machine_restart(char *cmd)
{
	/* Disable interrupts first */
	local_irq_disable();
	smp_send_stop();

	/*
	 * UpdateCapsule() depends on the system being reset via
	 * ResetSystem().
	 */
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_reboot(reboot_mode, NULL);

	/* Now call the architecture specific reboot code. */
	do_kernel_restart(cmd);

	/*
	 * Whoops - the architecture was unable to reboot.
	 */
	printk("Reboot failed -- System halted\n");
	while (1);
}

#define bstr(suffix, str) [PSR_BTYPE_ ## suffix >> PSR_BTYPE_SHIFT] = str
static const char *const btypes[] = {
	bstr(NONE, "--"),
	bstr(  JC, "jc"),
	bstr(   C, "-c"),
	bstr(  J , "j-")
};
#undef bstr

static void print_pstate(struct pt_regs *regs)
{
	u64 pstate = regs->pstate;

	if (compat_user_mode(regs)) {
		printk("pstate: %08llx (%c%c%c%c %c %s %s %c%c%c %cDIT %cSSBS)\n",
			pstate,
			pstate & PSR_AA32_N_BIT ? 'N' : 'n',
			pstate & PSR_AA32_Z_BIT ? 'Z' : 'z',
			pstate & PSR_AA32_C_BIT ? 'C' : 'c',
			pstate & PSR_AA32_V_BIT ? 'V' : 'v',
			pstate & PSR_AA32_Q_BIT ? 'Q' : 'q',
			pstate & PSR_AA32_T_BIT ? "T32" : "A32",
			pstate & PSR_AA32_E_BIT ? "BE" : "LE",
			pstate & PSR_AA32_A_BIT ? 'A' : 'a',
			pstate & PSR_AA32_I_BIT ? 'I' : 'i',
			pstate & PSR_AA32_F_BIT ? 'F' : 'f',
			pstate & PSR_AA32_DIT_BIT ? '+' : '-',
			pstate & PSR_AA32_SSBS_BIT ? '+' : '-');
	} else {
		const char *btype_str = btypes[(pstate & PSR_BTYPE_MASK) >>
					       PSR_BTYPE_SHIFT];

		printk("pstate: %08llx (%c%c%c%c %c%c%c%c %cPAN %cUAO %cTCO %cDIT %cSSBS BTYPE=%s)\n",
			pstate,
			pstate & PSR_N_BIT ? 'N' : 'n',
			pstate & PSR_Z_BIT ? 'Z' : 'z',
			pstate & PSR_C_BIT ? 'C' : 'c',
			pstate & PSR_V_BIT ? 'V' : 'v',
			pstate & PSR_D_BIT ? 'D' : 'd',
			pstate & PSR_A_BIT ? 'A' : 'a',
			pstate & PSR_I_BIT ? 'I' : 'i',
			pstate & PSR_F_BIT ? 'F' : 'f',
			pstate & PSR_PAN_BIT ? '+' : '-',
			pstate & PSR_UAO_BIT ? '+' : '-',
			pstate & PSR_TCO_BIT ? '+' : '-',
			pstate & PSR_DIT_BIT ? '+' : '-',
			pstate & PSR_SSBS_BIT ? '+' : '-',
			btype_str);
	}
}

void __show_regs(struct pt_regs *regs)
{
	int i, top_reg;
	u64 lr, sp;

	if (compat_user_mode(regs)) {
		lr = regs->compat_lr;
		sp = regs->compat_sp;
		top_reg = 12;
	} else {
		lr = regs->regs[30];
		sp = regs->sp;
		top_reg = 29;
	}

	show_regs_print_info(KERN_DEFAULT);
	print_pstate(regs);

	if (!user_mode(regs)) {
		printk("pc : %pS\n", (void *)regs->pc);
		printk("lr : %pS\n", (void *)ptrauth_strip_kernel_insn_pac(lr));
	} else {
		printk("pc : %016llx\n", regs->pc);
		printk("lr : %016llx\n", lr);
	}

	printk("sp : %016llx\n", sp);

	if (system_uses_irq_prio_masking())
		printk("pmr: %08x\n", regs->pmr);

	i = top_reg;

	while (i >= 0) {
		printk("x%-2d: %016llx", i, regs->regs[i]);

		while (i-- % 3)
			pr_cont(" x%-2d: %016llx", i, regs->regs[i]);

		pr_cont("\n");
	}
}

void show_regs(struct pt_regs *regs)
{
	__show_regs(regs);
	dump_backtrace(regs, NULL, KERN_DEFAULT);
}

static void tls_thread_flush(void)
{
	write_sysreg(0, tpidr_el0);
	if (system_supports_tpidr2())
		write_sysreg_s(0, SYS_TPIDR2_EL0);

	if (is_compat_task()) {
		current->thread.uw.tp_value = 0;

		/*
		 * We need to ensure ordering between the shadow state and the
		 * hardware state, so that we don't corrupt the hardware state
		 * with a stale shadow state during context switch.
		 */
		barrier();
		write_sysreg(0, tpidrro_el0);
	}
}

static void flush_tagged_addr_state(void)
{
	if (IS_ENABLED(CONFIG_ARM64_TAGGED_ADDR_ABI))
		clear_thread_flag(TIF_TAGGED_ADDR);
}

static void flush_poe(void)
{
	if (!system_supports_poe())
		return;

	write_sysreg_s(POR_EL0_INIT, SYS_POR_EL0);
}

#ifdef CONFIG_ARM64_GCS

static void flush_gcs(void)
{
	if (!system_supports_gcs())
		return;

	current->thread.gcspr_el0 = 0;
	current->thread.gcs_base = 0;
	current->thread.gcs_size = 0;
	current->thread.gcs_el0_mode = 0;
	write_sysreg_s(GCSCRE0_EL1_nTR, SYS_GCSCRE0_EL1);
	write_sysreg_s(0, SYS_GCSPR_EL0);
}

static int copy_thread_gcs(struct task_struct *p,
			   const struct kernel_clone_args *args)
{
	unsigned long gcs;

	if (!system_supports_gcs())
		return 0;

	p->thread.gcs_base = 0;
	p->thread.gcs_size = 0;

	gcs = gcs_alloc_thread_stack(p, args);
	if (IS_ERR_VALUE(gcs))
		return PTR_ERR((void *)gcs);

	p->thread.gcs_el0_mode = current->thread.gcs_el0_mode;
	p->thread.gcs_el0_locked = current->thread.gcs_el0_locked;

	return 0;
}

#else

static void flush_gcs(void) { }
static int copy_thread_gcs(struct task_struct *p,
			   const struct kernel_clone_args *args)
{
	return 0;
}

#endif

void flush_thread(void)
{
	fpsimd_flush_thread();
	tls_thread_flush();
	flush_ptrace_hw_breakpoint(current);
	flush_tagged_addr_state();
	flush_poe();
	flush_gcs();
}

void arch_release_task_struct(struct task_struct *tsk)
{
	fpsimd_release_task(tsk);
	gcs_free(tsk);
}

int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	/*
	 * The current/src task's FPSIMD state may or may not be live, and may
	 * have been altered by ptrace after entry to the kernel. Save the
	 * effective FPSIMD state so that this will be copied into dst.
	 */
	fpsimd_save_and_flush_current_state();
	fpsimd_sync_from_effective_state(src);

	*dst = *src;

	/*
	 * Drop stale reference to src's sve_state and convert dst to
	 * non-streaming FPSIMD mode.
	 */
	dst->thread.fp_type = FP_STATE_FPSIMD;
	dst->thread.sve_state = NULL;
	clear_tsk_thread_flag(dst, TIF_SVE);
	task_smstop_sm(dst);

	/*
	 * Drop stale reference to src's sme_state and ensure dst has ZA
	 * disabled.
	 *
	 * When necessary, ZA will be inherited later in copy_thread_za().
	 */
	dst->thread.sme_state = NULL;
	clear_tsk_thread_flag(dst, TIF_SME);
	dst->thread.svcr &= ~SVCR_ZA_MASK;

	/* clear any pending asynchronous tag fault raised by the parent */
	clear_tsk_thread_flag(dst, TIF_MTE_ASYNC_FAULT);

	return 0;
}

static int copy_thread_za(struct task_struct *dst, struct task_struct *src)
{
	if (!thread_za_enabled(&src->thread))
		return 0;

	dst->thread.sve_state = kzalloc(sve_state_size(src),
					GFP_KERNEL);
	if (!dst->thread.sve_state)
		return -ENOMEM;

	dst->thread.sme_state = kmemdup(src->thread.sme_state,
					sme_state_size(src),
					GFP_KERNEL);
	if (!dst->thread.sme_state) {
		kfree(dst->thread.sve_state);
		dst->thread.sve_state = NULL;
		return -ENOMEM;
	}

	set_tsk_thread_flag(dst, TIF_SME);
	dst->thread.svcr |= SVCR_ZA_MASK;

	return 0;
}

asmlinkage void ret_from_fork(void) asm("ret_from_fork");

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long clone_flags = args->flags;
	unsigned long stack_start = args->stack;
	unsigned long tls = args->tls;
	struct pt_regs *childregs = task_pt_regs(p);
	int ret;

	memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));

	/*
	 * In case p was allocated the same task_struct pointer as some
	 * other recently-exited task, make sure p is disassociated from
	 * any cpu that may have run that now-exited task recently.
	 * Otherwise we could erroneously skip reloading the FPSIMD
	 * registers for p.
	 */
	fpsimd_flush_task_state(p);

	ptrauth_thread_init_kernel(p);

	if (likely(!args->fn)) {
		*childregs = *current_pt_regs();
		childregs->regs[0] = 0;

		/*
		 * Read the current TLS pointer from tpidr_el0 as it may be
		 * out-of-sync with the saved value.
		 */
		*task_user_tls(p) = read_sysreg(tpidr_el0);

		if (system_supports_poe())
			p->thread.por_el0 = read_sysreg_s(SYS_POR_EL0);

		if (stack_start) {
			if (is_compat_thread(task_thread_info(p)))
				childregs->compat_sp = stack_start;
			else
				childregs->sp = stack_start;
		}

		/*
		 * Due to the AAPCS64 "ZA lazy saving scheme", PSTATE.ZA and
		 * TPIDR2 need to be manipulated as a pair, and either both
		 * need to be inherited or both need to be reset.
		 *
		 * Within a process, child threads must not inherit their
		 * parent's TPIDR2 value or they may clobber their parent's
		 * stack at some later point.
		 *
		 * When a process is fork()'d, the child must inherit ZA and
		 * TPIDR2 from its parent in case there was dormant ZA state.
		 *
		 * Use CLONE_VM to determine when the child will share the
		 * address space with the parent, and cannot safely inherit the
		 * state.
		 */
		if (system_supports_sme()) {
			if (!(clone_flags & CLONE_VM)) {
				p->thread.tpidr2_el0 = read_sysreg_s(SYS_TPIDR2_EL0);
				ret = copy_thread_za(p, current);
				if (ret)
					return ret;
			} else {
				p->thread.tpidr2_el0 = 0;
				WARN_ON_ONCE(p->thread.svcr & SVCR_ZA_MASK);
			}
		}

		/*
		 * If a TLS pointer was passed to clone, use it for the new
		 * thread.
		 */
		if (clone_flags & CLONE_SETTLS)
			p->thread.uw.tp_value = tls;

		ret = copy_thread_gcs(p, args);
		if (ret != 0)
			return ret;
	} else {
		/*
		 * A kthread has no context to ERET to, so ensure any buggy
		 * ERET is treated as an illegal exception return.
		 *
		 * When a user task is created from a kthread, childregs will
		 * be initialized by start_thread() or start_compat_thread().
		 */
		memset(childregs, 0, sizeof(struct pt_regs));
		childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;
		childregs->stackframe.type = FRAME_META_TYPE_FINAL;

		p->thread.cpu_context.x19 = (unsigned long)args->fn;
		p->thread.cpu_context.x20 = (unsigned long)args->fn_arg;

		if (system_supports_poe())
			p->thread.por_el0 = POR_EL0_INIT;
	}
	p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
	p->thread.cpu_context.sp = (unsigned long)childregs;
	/*
	 * For the benefit of the unwinder, set up childregs->stackframe
	 * as the final frame for the new task.
	 */
	p->thread.cpu_context.fp = (unsigned long)&childregs->stackframe;

	ptrace_hw_copy_thread(p);

	return 0;
}

void tls_preserve_current_state(void)
{
	*task_user_tls(current) = read_sysreg(tpidr_el0);
	if (system_supports_tpidr2() && !is_compat_task())
		current->thread.tpidr2_el0 = read_sysreg_s(SYS_TPIDR2_EL0);
}

static void tls_thread_switch(struct task_struct *next)
{
	tls_preserve_current_state();

	if (is_compat_thread(task_thread_info(next)))
		write_sysreg(next->thread.uw.tp_value, tpidrro_el0);
	else
		write_sysreg(0, tpidrro_el0);

	write_sysreg(*task_user_tls(next), tpidr_el0);
	if (system_supports_tpidr2())
		write_sysreg_s(next->thread.tpidr2_el0, SYS_TPIDR2_EL0);
}

/*
 * Force SSBS state on context-switch, since it may be lost after migrating
 * from a CPU which treats the bit as RES0 in a heterogeneous system.
 */
static void ssbs_thread_switch(struct task_struct *next)
{
	/*
	 * Nothing to do for kernel threads, but 'regs' may be junk
	 * (e.g. idle task) so check the flags and bail early.
	 */
	if (unlikely(next->flags & PF_KTHREAD))
		return;

	/*
	 * If all CPUs implement the SSBS extension, then we just need to
	 * context-switch the PSTATE field.
	 */
	if (alternative_has_cap_unlikely(ARM64_SSBS))
		return;

	spectre_v4_enable_task_mitigation(next);
}

/*
 * We store our current task in sp_el0, which is clobbered by userspace. Keep a
 * shadow copy so that we can restore this upon entry from userspace.
 *
 * This is *only* for exception entry from EL0, and is not valid until we
 * __switch_to() a user task.
 */
DEFINE_PER_CPU(struct task_struct *, __entry_task);

static void entry_task_switch(struct task_struct *next)
{
	__this_cpu_write(__entry_task, next);
}

#ifdef CONFIG_ARM64_GCS

void gcs_preserve_current_state(void)
{
	current->thread.gcspr_el0 = read_sysreg_s(SYS_GCSPR_EL0);
}

static void gcs_thread_switch(struct task_struct *next)
{
	if (!system_supports_gcs())
		return;

	/* GCSPR_EL0 is always readable */
	gcs_preserve_current_state();
	write_sysreg_s(next->thread.gcspr_el0, SYS_GCSPR_EL0);

	if (current->thread.gcs_el0_mode != next->thread.gcs_el0_mode)
		gcs_set_el0_mode(next);

	/*
	 * Ensure that GCS memory effects of the 'prev' thread are
	 * ordered before other memory accesses with release semantics
	 * (or preceded by a DMB) on the current PE. In addition, any
	 * memory accesses with acquire semantics (or succeeded by a
	 * DMB) are ordered before GCS memory effects of the 'next'
	 * thread. This will ensure that the GCS memory effects are
	 * visible to other PEs in case of migration.
	 */
	if (task_gcs_el0_enabled(current) || task_gcs_el0_enabled(next))
		gcsb_dsync();
}

#else

static void gcs_thread_switch(struct task_struct *next)
{
}

#endif

/*
 * Handle sysreg updates for ARM erratum 1418040 which affects the 32bit view of
 * CNTVCT, various other errata which require trapping all CNTVCT{,_EL0}
 * accesses and prctl(PR_SET_TSC). Ensure access is disabled iff a workaround is
 * required or PR_TSC_SIGSEGV is set.
 */
static void update_cntkctl_el1(struct task_struct *next)
{
	struct thread_info *ti = task_thread_info(next);

	if (test_ti_thread_flag(ti, TIF_TSC_SIGSEGV) ||
	    has_erratum_handler(read_cntvct_el0) ||
	    (IS_ENABLED(CONFIG_ARM64_ERRATUM_1418040) &&
	     this_cpu_has_cap(ARM64_WORKAROUND_1418040) &&
	     is_compat_thread(ti)))
		sysreg_clear_set(cntkctl_el1, ARCH_TIMER_USR_VCT_ACCESS_EN, 0);
	else
		sysreg_clear_set(cntkctl_el1, 0, ARCH_TIMER_USR_VCT_ACCESS_EN);
}

static void cntkctl_thread_switch(struct task_struct *prev,
				  struct task_struct *next)
{
	if ((read_ti_thread_flags(task_thread_info(prev)) &
	     (_TIF_32BIT | _TIF_TSC_SIGSEGV)) !=
	    (read_ti_thread_flags(task_thread_info(next)) &
	     (_TIF_32BIT | _TIF_TSC_SIGSEGV)))
		update_cntkctl_el1(next);
}

static int do_set_tsc_mode(unsigned int val)
{
	bool tsc_sigsegv;

	if (val == PR_TSC_SIGSEGV)
		tsc_sigsegv = true;
	else if (val == PR_TSC_ENABLE)
		tsc_sigsegv = false;
	else
		return -EINVAL;

	preempt_disable();
	update_thread_flag(TIF_TSC_SIGSEGV, tsc_sigsegv);
	update_cntkctl_el1(current);
	preempt_enable();

	return 0;
}

static void permission_overlay_switch(struct task_struct *next)
{
	if (!system_supports_poe())
		return;

	current->thread.por_el0 = read_sysreg_s(SYS_POR_EL0);
	if (current->thread.por_el0 != next->thread.por_el0) {
		write_sysreg_s(next->thread.por_el0, SYS_POR_EL0);
		/*
		 * No ISB required as we can tolerate spurious Overlay faults -
		 * the fault handler will check again based on the new value
		 * of POR_EL0.
		 */
	}
}

/*
 * __switch_to() checks current->thread.sctlr_user as an optimisation. Therefore
 * this function must be called with preemption disabled and the update to
 * sctlr_user must be made in the same preemption disabled block so that
 * __switch_to() does not see the variable update before the SCTLR_EL1 one.
 */
void update_sctlr_el1(u64 sctlr)
{
	/*
	 * EnIA must not be cleared while in the kernel as this is necessary for
	 * in-kernel PAC. It will be cleared on kernel exit if needed.
	 */
	sysreg_clear_set(sctlr_el1, SCTLR_USER_MASK & ~SCTLR_ELx_ENIA, sctlr);

	/* ISB required for the kernel uaccess routines when setting TCF0. */
	isb();
}

/*
 * Thread switching.
 */
__notrace_funcgraph __sched
struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct task_struct *last;

	fpsimd_thread_switch(next);
	tls_thread_switch(next);
	hw_breakpoint_thread_switch(next);
	contextidr_thread_switch(next);
	entry_task_switch(next);
	ssbs_thread_switch(next);
	cntkctl_thread_switch(prev, next);
	ptrauth_thread_switch_user(next);
	permission_overlay_switch(next);
	gcs_thread_switch(next);

	/*
	 * Complete any pending TLB or cache maintenance on this CPU in case the
	 * thread migrates to a different CPU. This full barrier is also
	 * required by the membarrier system call. Additionally it makes any
	 * in-progress pgtable writes visible to the table walker; See
	 * emit_pte_barriers().
	 */
	dsb(ish);

	/*
	 * MTE thread switching must happen after the DSB above to ensure that
	 * any asynchronous tag check faults have been logged in the TFSR*_EL1
	 * registers.
	 */
	mte_thread_switch(next);
	/* avoid expensive SCTLR_EL1 accesses if no change */
	if (prev->thread.sctlr_user != next->thread.sctlr_user)
		update_sctlr_el1(next->thread.sctlr_user);

	/* the actual thread switch */
	last = cpu_switch_to(prev, next);

	return last;
}

struct wchan_info {
	unsigned long	pc;
	int		count;
};

static bool get_wchan_cb(void *arg, unsigned long pc)
{
	struct wchan_info *wchan_info = arg;

	if (!in_sched_functions(pc)) {
		wchan_info->pc = pc;
		return false;
	}
	return wchan_info->count++ < 16;
}

unsigned long __get_wchan(struct task_struct *p)
{
	struct wchan_info wchan_info = {
		.pc = 0,
		.count = 0,
	};

	if (!try_get_task_stack(p))
		return 0;

	arch_stack_walk(get_wchan_cb, &wchan_info, p, NULL);

	put_task_stack(p);

	return wchan_info.pc;
}

unsigned long arch_align_stack(unsigned long sp)
{
	if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
		sp -= get_random_u32_below(PAGE_SIZE);
	return sp & ~0xf;
}

#ifdef CONFIG_COMPAT
int compat_elf_check_arch(const struct elf32_hdr *hdr)
{
	if (!system_supports_32bit_el0())
		return false;

	if ((hdr)->e_machine != EM_ARM)
		return false;

	if (!((hdr)->e_flags & EF_ARM_EABI_MASK))
		return false;

	/*
	 * Prevent execve() of a 32-bit program from a deadline task
	 * if the restricted affinity mask would be inadmissible on an
	 * asymmetric system.
	 */
	return !static_branch_unlikely(&arm64_mismatched_32bit_el0) ||
	       !dl_task_check_affinity(current, system_32bit_el0_cpumask());
}
#endif

/*
 * Called from setup_new_exec() after (COMPAT_)SET_PERSONALITY.
 */
void arch_setup_new_exec(void)
{
	unsigned long mmflags = 0;

	if (is_compat_task()) {
		mmflags = MMCF_AARCH32;

		/*
		 * Restrict the CPU affinity mask for a 32-bit task so that
		 * it contains only 32-bit-capable CPUs.
		 *
		 * From the perspective of the task, this looks similar to
		 * what would happen if the 64-bit-only CPUs were hot-unplugged
		 * at the point of execve(), although we try a bit harder to
		 * honour the cpuset hierarchy.
		 */
		if (static_branch_unlikely(&arm64_mismatched_32bit_el0))
			force_compatible_cpus_allowed_ptr(current);
	} else if (static_branch_unlikely(&arm64_mismatched_32bit_el0)) {
		relax_compatible_cpus_allowed_ptr(current);
	}

	current->mm->context.flags = mmflags;
	ptrauth_thread_init_user();
	mte_thread_init_user();
	do_set_tsc_mode(PR_TSC_ENABLE);

	if (task_spec_ssb_noexec(current)) {
		arch_prctl_spec_ctrl_set(current, PR_SPEC_STORE_BYPASS,
					 PR_SPEC_ENABLE);
	}
}

#ifdef CONFIG_ARM64_TAGGED_ADDR_ABI
/*
 * Control the relaxed ABI allowing tagged user addresses into the kernel.
 */
static unsigned int tagged_addr_disabled;

long set_tagged_addr_ctrl(struct task_struct *task, unsigned long arg)
{
	unsigned long valid_mask = PR_TAGGED_ADDR_ENABLE;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (system_supports_mte())
		valid_mask |= PR_MTE_TCF_SYNC | PR_MTE_TCF_ASYNC \
			| PR_MTE_TAG_MASK;

	if (arg & ~valid_mask)
		return -EINVAL;

	/*
	 * Do not allow the enabling of the tagged address ABI if globally
	 * disabled via sysctl abi.tagged_addr_disabled.
	 */
	if (arg & PR_TAGGED_ADDR_ENABLE && tagged_addr_disabled)
		return -EINVAL;

	if (set_mte_ctrl(task, arg) != 0)
		return -EINVAL;

	update_ti_thread_flag(ti, TIF_TAGGED_ADDR, arg & PR_TAGGED_ADDR_ENABLE);

	return 0;
}

long get_tagged_addr_ctrl(struct task_struct *task)
{
	long ret = 0;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (test_ti_thread_flag(ti, TIF_TAGGED_ADDR))
		ret = PR_TAGGED_ADDR_ENABLE;

	ret |= get_mte_ctrl(task);

	return ret;
}

/*
 * Global sysctl to disable the tagged user addresses support. This control
 * only prevents the tagged address ABI enabling via prctl() and does not
 * disable it for tasks that already opted in to the relaxed ABI.
 */

static const struct ctl_table tagged_addr_sysctl_table[] = {
	{
		.procname	= "tagged_addr_disabled",
		.mode		= 0644,
		.data		= &tagged_addr_disabled,
		.maxlen		= sizeof(int),
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static int __init tagged_addr_init(void)
{
	if (!register_sysctl("abi", tagged_addr_sysctl_table))
		return -EINVAL;
	return 0;
}

core_initcall(tagged_addr_init);
#endif	/* CONFIG_ARM64_TAGGED_ADDR_ABI */

#ifdef CONFIG_BINFMT_ELF
int arch_elf_adjust_prot(int prot, const struct arch_elf_state *state,
			 bool has_interp, bool is_interp)
{
	/*
	 * For dynamically linked executables the interpreter is
	 * responsible for setting PROT_BTI on everything except
	 * itself.
	 */
	if (is_interp != has_interp)
		return prot;

	if (!(state->flags & ARM64_ELF_BTI))
		return prot;

	if (prot & PROT_EXEC)
		prot |= PROT_BTI;

	return prot;
}
#endif

int get_tsc_mode(unsigned long adr)
{
	unsigned int val;

	if (is_compat_task())
		return -EINVAL;

	if (test_thread_flag(TIF_TSC_SIGSEGV))
		val = PR_TSC_SIGSEGV;
	else
		val = PR_TSC_ENABLE;

	return put_user(val, (unsigned int __user *)adr);
}

int set_tsc_mode(unsigned int val)
{
	if (is_compat_task())
		return -EINVAL;

	return do_set_tsc_mode(val);
}
