#include "debug.h"
#include <signal.h>
#include "sys/calls.h"
#include "sys/signal.h"
#include "sys/vdso.h"

int xsave_extra = 0;
int fxsave_extra = 0;

void send_signal(struct process *proc, int sig) {
    lock(proc);
    if (proc->sigactions[sig].handler != SIG_IGN_) {
        if (proc->blocked & (1 << sig)) {
            proc->queued |= (1 << sig);
            unlock(proc);
        } else {
            proc->pending |= (1 << sig);
            unlock(proc);
            pthread_kill(proc->thread, SIGUSR1);
        }
    }
}

static void receive_signal(int sig) {
    if (current->sigactions[sig].handler == SIG_DFL_) {
        do_exit(sig);
    }

    // setup the frame
    struct sigframe_ frame = {};
    frame.sig = sig;

    struct cpu_state *cpu = &current->cpu;
    frame.sc.ax = cpu->eax;
    frame.sc.bx = cpu->ebx;
    frame.sc.cx = cpu->ecx;
    frame.sc.dx = cpu->edx;
    frame.sc.di = cpu->edi;
    frame.sc.si = cpu->esi;
    frame.sc.bp = cpu->ebp;
    frame.sc.sp = frame.sc.sp_at_signal = cpu->esp;
    frame.sc.ip = cpu->eip;
    collapse_flags(cpu);
    frame.sc.flags = cpu->eflags;
    frame.sc.trapno = cpu->trapno;
    // TODO more shit

    addr_t sigreturn_addr = vdso_symbol("__kernel_rt_sigreturn");
    if (sigreturn_addr == 0) {
        fprintf(stderr, "sigreturn not found in vdso, this should never happen\n");
        abort();
    }
    frame.pretcode = current->vdso + sigreturn_addr;
    // for legacy purposes
    frame.retcode.popmov = 0xb858;
    frame.retcode.nr_sigreturn = 173; // rt_sigreturn
    frame.retcode.int80 = 0x80cd;

    // set up registers for signal handler
    cpu->eax = sig;
    cpu->eip = current->sigactions[sig].handler;

    dword_t sp = cpu->esp;
    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
    sp -= sizeof(struct sigframe_);
    // align sp + 4 on a 16-byte boundary because that's what the abi says
	sp = ((sp + 4) & ~0xf) - 4;
    cpu->esp = sp;

    // install frame
    // nothing we can do if this fails
    // TODO do something other than nothing, like printk maybe
    (void) user_put(sp, frame);

    current->queued &= ~(1 << sig);
}

void receive_signals() {
    lock(current);
    if (current->pending) {
        for (int sig = 0; sig < NUM_SIGS; sig++)
            if (current->pending & (1 << sig))
                receive_signal(sig);
    }
    unlock(current);
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (sig == SIGKILL_ || sig == SIGSTOP_)
        return _EINVAL;

    lock(current);
    if (oldaction)
        *oldaction = current->sigactions[sig];
    if (action)
        current->sigactions[sig] = *action;
    unlock(current);
    return 0;
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;
    struct sigaction_ action, oldaction;
    if (action_addr != 0)
        if (user_get(action_addr, action))
            return _EFAULT;
    if (oldaction_addr != 0)
        if (user_get(oldaction_addr, oldaction))
            return _EFAULT;

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0)
        if (user_put(oldaction_addr, oldaction))
            return _EFAULT;
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    // only allow musl's funky thing it does in sigaction
    if (size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    sigset_t_ oldset = current->blocked;

    lock(current);
    if (how == SIG_BLOCK_)
        current->blocked |= set;
    else if (how == SIG_UNBLOCK_)
        current->blocked &= ~set;
    else if (how == SIG_SETMASK_)
        current->blocked = set;
    else {
        unlock(current);
        return _EINVAL;
    }

    // transfer unblocked signals from queued to pending
    sigset_t_ unblocked = oldset & ~current->blocked;
    current->pending |= current->queued & unblocked;
    current->queued &= ~unblocked;
    unlock(current);

    if (oldset_addr != 0)
        if (user_put(oldset_addr, oldset))
            return _EFAULT;
    return 0;
}