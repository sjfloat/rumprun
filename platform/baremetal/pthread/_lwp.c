/* 
 * Copyright (c) 2014 Antti Kantee
 */

/* XXX: silly */
#define _lwp_park ___lwp_park60

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/lwpctl.h>
#include <sys/lwp.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/tls.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bmk/sched.h>

#include "pthread_makelwp.h"

#if 0
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

struct schedulable {
	struct tls_tcb scd_tls;

	struct bmk_thread *scd_thread;
	int scd_lwpid;
	char *scd_name;

	struct lwpctl scd_lwpctl;

	TAILQ_ENTRY(schedulable) entries;
};
static TAILQ_HEAD(, schedulable) scheds = TAILQ_HEAD_INITIALIZER(scheds);

static struct schedulable mainthread = {
	.scd_lwpid = 1,
};
struct tls_tcb *curtcb = &mainthread.scd_tls;

struct tls_tcb *_lwp_rumpbaremetal_gettcb(void);
struct tls_tcb *
_lwp_rumpbaremetal_gettcb(void)
{

	return curtcb;
}

int
_lwp_ctl(int ctl, struct lwpctl **data)
{
	struct schedulable *scd = (struct schedulable *)curtcb;

	*data = (struct lwpctl *)&scd->scd_lwpctl;
	return 0;
}

int
pthread__makelwp(void (*start)(void *), void *arg, void *private,
	void *stack_base, size_t stack_size, unsigned long flag, lwpid_t *lid)
{
	struct schedulable *scd = private;
	static int nextlid = 2;
	*lid = nextlid++;

	scd->scd_lwpid = *lid;
	scd->scd_thread = bmk_sched_create("lwp", scd, 0, start, arg,
	    stack_base, stack_size);
	if (scd->scd_thread == NULL)
		return EBUSY; /* ??? */
	TAILQ_INSERT_TAIL(&scheds, scd, entries);

	return 0;
}

static struct schedulable *
lwpid2scd(lwpid_t lid)
{
	struct schedulable *scd;

	TAILQ_FOREACH(scd, &scheds, entries) {
		if (scd->scd_lwpid == lid)
			return scd;
	}
	return NULL;
}

int
_lwp_unpark(lwpid_t lid, const void *hint)
{
	struct schedulable *scd;

	DPRINTF(("lwp unpark %d\n", lid));
	if ((scd = lwpid2scd(lid)) == NULL) {
		return -1;
	}

	bmk_sched_wake(scd->scd_thread);
	return 0;
}

ssize_t
_lwp_unpark_all(const lwpid_t *targets, size_t ntargets, const void *hint)
{
	ssize_t rv;

	if (targets == NULL)
		return 1024;

	/*
	 * XXX: this it not 100% correct (unmarking has memory), but good
	 * enuf for now
	 */
	rv = ntargets;
	while (ntargets--) {
		if (_lwp_unpark(*targets, NULL) != 0)
			rv--;
		targets++;
	}
	//assert(rv >= 0);
	return rv;
}

/*
 * called by the scheduler when a context switch is made
 * nb. cookie is null when non-lwp threads are being run
 */
static void
schedhook(void *prevcookie, void *nextcookie)
{
	struct schedulable *prev, *scd;

	scd = nextcookie;
	curtcb = nextcookie;
	prev = prevcookie;

	if (prev && prev->scd_lwpctl.lc_curcpu != LWPCTL_CPU_EXITED) {
		prev->scd_lwpctl.lc_curcpu = LWPCTL_CPU_NONE;
	}
	if (scd) {
		scd->scd_lwpctl.lc_curcpu = 0;
		scd->scd_lwpctl.lc_pctr++;
	}
}

void
rumprun_lwp_init(void)
{

	bmk_sched_set_hook(schedhook);
	mainthread.scd_thread = bmk_sched_init_mainthread(&mainthread.scd_tls);
	TAILQ_INSERT_TAIL(&scheds, &mainthread, entries);
}

int
_lwp_park(clockid_t clock_id, int flags, const struct timespec *ts,
	lwpid_t unpark, const void *hint, const void *unparkhint)
{
	struct schedulable *current = (struct schedulable *)curtcb;
	int rv;

	if (unpark)
		_lwp_unpark(unpark, unparkhint);

	if (ts) {
		long nsec = ts->tv_sec*1000*1000*1000 + ts->tv_nsec;
		if (bmk_sched_nanosleep(nsec))
			rv = ETIMEDOUT;
		else
			rv = 0;
	} else {
		bmk_sched_block(current->scd_thread);
		bmk_sched();
		rv = 0;
	}

	if (rv) {
		errno = rv;
		rv = -1;
	}
	return rv;
}

int
_lwp_exit(void)
{
	struct schedulable *scd = (struct schedulable *)curtcb;

	scd->scd_lwpctl.lc_curcpu = LWPCTL_CPU_EXITED;
	TAILQ_REMOVE(&scheds, scd, entries);
	bmk_sched_exit();

	return 0;
}

int
_lwp_continue(lwpid_t lid)
{
	struct schedulable *scd;

	if ((scd = lwpid2scd(lid)) == NULL) {
		return ESRCH;
	}

	bmk_sched_wake(scd->scd_thread);
	return 0;
}

int
_lwp_suspend(lwpid_t lid)
{
	struct schedulable *scd;

	if ((scd = lwpid2scd(lid)) == NULL) {
		return ESRCH;
	}

	bmk_sched_block(scd->scd_thread);
	return 0;
}

int
_lwp_wakeup(lwpid_t lid)
{
	struct schedulable *scd;

	if ((scd = lwpid2scd(lid)) == NULL)
		return ESRCH;

	bmk_sched_wake(scd->scd_thread);
	return ENODEV;
}

int
_lwp_setname(lwpid_t lid, const char *name)
{
	struct schedulable *scd;
	char *newname, *oldname;
	size_t nlen;

	if ((scd = lwpid2scd(lid)) == NULL)
		return ESRCH;

	nlen = strlen(name)+1;
	if (nlen > MAXCOMLEN)
		nlen = MAXCOMLEN;
	newname = malloc(nlen);
	if (newname == NULL)
		return ENOMEM;
	memcpy(newname, name, nlen-1);
	newname[nlen-1] = '\0';

	oldname = scd->scd_name;
	scd->scd_name = newname;
	if (oldname) {
		free(oldname);
	}

	return 0;
}

lwpid_t
_lwp_self(void)
{
	struct schedulable *current = (struct schedulable *)curtcb;

	return current->scd_lwpid;
}

int
sched_yield(void)
{

	bmk_sched();
	return 0;
}
__strong_alias(_sched_yield,sched_yield);
__strong_alias(_sys_sched_yield,_sched_yield);

struct tls_tcb *
_rtld_tls_allocate(void)
{

	return malloc(sizeof(struct schedulable));
}

void
_rtld_tls_free(struct tls_tcb *arg)
{

	free(arg);
}

void _lwpnullop(void);
void _lwpnullop(void) { }

__strong_alias(_sys_setcontext,_lwpnullop);
__strong_alias(___sigprocmask14,_lwpnullop);

__strong_alias(_sys___sigprocmask14,_lwpnullop);

__strong_alias(pthread__cancel_stub_binder,_lwpnullop);

__strong_alias(__libc_static_tls_setup,_lwpnullop);

int rasctl(void);
int rasctl(void) { return ENOSYS; }

/*
 * There is ongoing work to support these in the rump kernel,
 * so I will just stub them out for now.
 */
__strong_alias(_sched_getaffinity,_lwpnullop);
__strong_alias(_sched_getparam,_lwpnullop);
__strong_alias(_sched_setaffinity,_lwpnullop);
__strong_alias(_sched_setparam,_lwpnullop);
