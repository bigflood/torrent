// Copyright 2011 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// +build darwin nacl netbsd openbsd plan9 solaris windows

#include "runtime.h"
#include "stack.h"
#include "../../cmd/ld/textflag.h"

// This implementation depends on OS-specific implementations of
//
//	uintptr runtime·semacreate(void)
//		Create a semaphore, which will be assigned to m->waitsema.
//		The zero value is treated as absence of any semaphore,
//		so be sure to return a non-zero value.
//
//	int32 runtime·semasleep(int64 ns)
//		If ns < 0, acquire m->waitsema and return 0.
//		If ns >= 0, try to acquire m->waitsema for at most ns nanoseconds.
//		Return 0 if the semaphore was acquired, -1 if interrupted or timed out.
//
//	int32 runtime·semawakeup(M *mp)
//		Wake up mp, which is or will soon be sleeping on mp->waitsema.
//

enum
{
	LOCKED = 1,

	ACTIVE_SPIN = 4,
	ACTIVE_SPIN_CNT = 30,
	PASSIVE_SPIN = 1,
};

void
runtime·lock(Mutex *l)
{
	uintptr v;
	uint32 i, spin;

	if(g->m->locks++ < 0)
		runtime·throw("runtime·lock: lock count");

	// Speculative grab for lock.
	if(runtime·casp((void**)&l->key, nil, (void*)LOCKED))
		return;

	if(g->m->waitsema == 0)
		g->m->waitsema = runtime·semacreate();

	// On uniprocessor's, no point spinning.
	// On multiprocessors, spin for ACTIVE_SPIN attempts.
	spin = 0;
	if(runtime·ncpu > 1)
		spin = ACTIVE_SPIN;

	for(i=0;; i++) {
		v = (uintptr)runtime·atomicloadp((void**)&l->key);
		if((v&LOCKED) == 0) {
unlocked:
			if(runtime·casp((void**)&l->key, (void*)v, (void*)(v|LOCKED)))
				return;
			i = 0;
		}
		if(i<spin)
			runtime·procyield(ACTIVE_SPIN_CNT);
		else if(i<spin+PASSIVE_SPIN)
			runtime·osyield();
		else {
			// Someone else has it.
			// l->waitm points to a linked list of M's waiting
			// for this lock, chained through m->nextwaitm.
			// Queue this M.
			for(;;) {
				g->m->nextwaitm = (void*)(v&~LOCKED);
				if(runtime·casp((void**)&l->key, (void*)v, (void*)((uintptr)g->m|LOCKED)))
					break;
				v = (uintptr)runtime·atomicloadp((void**)&l->key);
				if((v&LOCKED) == 0)
					goto unlocked;
			}
			if(v&LOCKED) {
				// Queued.  Wait.
				runtime·semasleep(-1);
				i = 0;
			}
		}
	}
}

void
runtime·unlock(Mutex *l)
{
	uintptr v;
	M *mp;

	for(;;) {
		v = (uintptr)runtime·atomicloadp((void**)&l->key);
		if(v == LOCKED) {
			if(runtime·casp((void**)&l->key, (void*)LOCKED, nil))
				break;
		} else {
			// Other M's are waiting for the lock.
			// Dequeue an M.
			mp = (void*)(v&~LOCKED);
			if(runtime·casp((void**)&l->key, (void*)v, mp->nextwaitm)) {
				// Dequeued an M.  Wake it.
				runtime·semawakeup(mp);
				break;
			}
		}
	}

	if(--g->m->locks < 0)
		runtime·throw("runtime·unlock: lock count");
	if(g->m->locks == 0 && g->preempt)  // restore the preemption request in case we've cleared it in newstack
		g->stackguard0 = StackPreempt;
}

// One-time notifications.
void
runtime·noteclear(Note *n)
{
	n->key = 0;
}

void
runtime·notewakeup(Note *n)
{
	M *mp;

	do
		mp = runtime·atomicloadp((void**)&n->key);
	while(!runtime·casp((void**)&n->key, mp, (void*)LOCKED));

	// Successfully set waitm to LOCKED.
	// What was it before?
	if(mp == nil) {
		// Nothing was waiting.  Done.
	} else if(mp == (M*)LOCKED) {
		// Two notewakeups!  Not allowed.
		runtime·throw("notewakeup - double wakeup");
	} else {
		// Must be the waiting m.  Wake it up.
		runtime·semawakeup(mp);
	}
}

void
runtime·notewakeup_m(void)
{
	Note *n;

	n = g->m->ptrarg[0];
	g->m->ptrarg[0] = nil;
	runtime·notewakeup(n);
}

void
runtime·notesleep(Note *n)
{
	if(g != g->m->g0)
		runtime·throw("notesleep not on g0");

	if(g->m->waitsema == 0)
		g->m->waitsema = runtime·semacreate();
	if(!runtime·casp((void**)&n->key, nil, g->m)) {  // must be LOCKED (got wakeup)
		if(n->key != LOCKED)
			runtime·throw("notesleep - waitm out of sync");
		return;
	}
	// Queued.  Sleep.
	g->m->blocked = true;
	runtime·semasleep(-1);
	g->m->blocked = false;
}

#pragma textflag NOSPLIT
static bool
notetsleep(Note *n, int64 ns, int64 deadline, M *mp)
{
	// Conceptually, deadline and mp are local variables.
	// They are passed as arguments so that the space for them
	// does not count against our nosplit stack sequence.

	// Register for wakeup on n->waitm.
	if(!runtime·casp((void**)&n->key, nil, g->m)) {  // must be LOCKED (got wakeup already)
		if(n->key != LOCKED)
			runtime·throw("notetsleep - waitm out of sync");
		return true;
	}

	if(ns < 0) {
		// Queued.  Sleep.
		g->m->blocked = true;
		runtime·semasleep(-1);
		g->m->blocked = false;
		return true;
	}

	deadline = runtime·nanotime() + ns;
	for(;;) {
		// Registered.  Sleep.
		g->m->blocked = true;
		if(runtime·semasleep(ns) >= 0) {
			g->m->blocked = false;
			// Acquired semaphore, semawakeup unregistered us.
			// Done.
			return true;
		}
		g->m->blocked = false;

		// Interrupted or timed out.  Still registered.  Semaphore not acquired.
		ns = deadline - runtime·nanotime();
		if(ns <= 0)
			break;
		// Deadline hasn't arrived.  Keep sleeping.
	}

	// Deadline arrived.  Still registered.  Semaphore not acquired.
	// Want to give up and return, but have to unregister first,
	// so that any notewakeup racing with the return does not
	// try to grant us the semaphore when we don't expect it.
	for(;;) {
		mp = runtime·atomicloadp((void**)&n->key);
		if(mp == g->m) {
			// No wakeup yet; unregister if possible.
			if(runtime·casp((void**)&n->key, mp, nil))
				return false;
		} else if(mp == (M*)LOCKED) {
			// Wakeup happened so semaphore is available.
			// Grab it to avoid getting out of sync.
			g->m->blocked = true;
			if(runtime·semasleep(-1) < 0)
				runtime·throw("runtime: unable to acquire - semaphore out of sync");
			g->m->blocked = false;
			return true;
		} else
			runtime·throw("runtime: unexpected waitm - semaphore out of sync");
	}
}

bool
runtime·notetsleep(Note *n, int64 ns)
{
	bool res;

	if(g != g->m->g0 && !g->m->gcing)
		runtime·throw("notetsleep not on g0");

	if(g->m->waitsema == 0)
		g->m->waitsema = runtime·semacreate();

	res = notetsleep(n, ns, 0, nil);
	return res;
}

// same as runtime·notetsleep, but called on user g (not g0)
// calls only nosplit functions between entersyscallblock/exitsyscall
bool
runtime·notetsleepg(Note *n, int64 ns)
{
	bool res;

	if(g == g->m->g0)
		runtime·throw("notetsleepg on g0");

	if(g->m->waitsema == 0)
		g->m->waitsema = runtime·semacreate();

	runtime·entersyscallblock();
	res = notetsleep(n, ns, 0, nil);
	runtime·exitsyscall();
	return res;
}

void
runtime·notetsleepg_m(void)
{
	Note *n;
	int64 ns;

	n = g->m->ptrarg[0];
	g->m->ptrarg[0] = nil;
	ns = g->m->scalararg[0] + ((int64)g->m->scalararg[1] << 32);

	if(g->m->waitsema == 0)
		g->m->waitsema = runtime·semacreate();

	runtime·entersyscallblock_m();
	notetsleep(n, ns, 0, nil);
	// caller will call exitsyscall on g stack
	runtime·gogo(&g->m->curg->sched);
}