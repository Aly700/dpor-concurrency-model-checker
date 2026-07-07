# Invariants — DPOR Model Checker

## Soundness

- The checker may explore too many schedules, but must not skip a schedule class that can expose a distinct bug.
- Independence may only be claimed for actions whose ordering cannot affect future enabledness or observable state.
- A report of safe means no explored state and no pruned equivalent class contains a bug under the modeled semantics.
- With backward branches, safety is relative to the configured per-thread step
  bound. A result with no race/deadlock/error/assertion but a nonzero
  `bound_exceeded_executions` count is clean only up to that bound, never
  unconditionally clean.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes.
- Replaying a schedule must produce the same state, report, and trace.
- Labels are pseudo-actions and are never scheduled; replay validates each
  step against the normalized executable pc after skipping labels.
- Test programs must not depend on OS thread scheduling.

## Happens-before

- Release/acquire edges must update vector clocks consistently.
- Atomic operations model acquire loads, release stores, acquire-release RMW,
  and SC-per-location only; no relaxed orderings exist in this IR.
- `AtomicStore(address)` must replace that address's atomic location clock with
  the storing thread's post-tick clock and must not acquire or join the prior
  location clock. Extra happens-before edges can hide real races.
- `AtomicLoad(address)` must join the loading thread with that address's atomic
  location clock and must not mutate the location clock.
- `AtomicRmw(address)` must join the thread with the location clock and then
  replace the location clock with the joined thread clock, preserving the
  modeled release sequence without accumulating unrelated stores.
- A successful `CAS(address)` must use the same acquire-release join-then-
  replace clock semantics as `AtomicRmw`. A failed `CAS(address)` must be an
  acquire load only: it joins the location clock and must not replace it.
  Replacing on failure can fabricate a release edge from the failing thread and
  hide real races.
- A successful `Spawn(target)` must mark a previously not-started target as
  started and join the target thread's vector clock with the spawner's
  post-tick vector clock before any target action can execute.
- A successful `Join(target)` must join the caller with the target thread's
  final vector clock before any post-join action can execute.
- `Wait(cv, mutex)` must release `mutex` with the same clock update as
  `Unlock`; after wakeup it must reacquire `mutex` with the same clock join as
  `Lock` before advancing past the wait action.
- `Signal(cv)` and `Broadcast(cv)` must add a happens-before edge from the
  signaling thread to every waiter they wake. They must not queue permits when
  no waiter exists.
- Conflicting memory accesses unordered by happens-before are races.
- Values are deterministic schedule-order int64 cell values. Plain reads have
  no weak-memory value semantics: in racy programs they observe the value
  produced by the explored interleaving, and the race report is the bug.
- Atomic/atomic accesses are never races. Mixed plain/atomic same-address
  accesses are races when unordered by happens-before and at least one side is
  write-like: plain write, atomic store, successful CAS, or atomic RMW. CAS is
  still dependent as an atomic RMW regardless of runtime success.
- Register-only actions (`set`, `bnz`, `assert`) are thread-local and may be
  independent of every other thread's transition; same-thread program order is
  still never commuted.
- Assertion failures are first-class terminal reports with replayable,
  minimized schedules. They must not be downgraded to modeled errors.
- Deadlock detection must distinguish a true cycle from a voluntarily finished
  thread, and must identify whether each unfinished disabled thread is waiting
  on a mutex, a join target, or a condition variable.
- Thread completion is started-aware: a not-started static thread body is not
  finished for `Join` enabledness, even if the body is empty, but unstarted
  and unjoined bodies do not by themselves make clean termination a deadlock.
- A terminal state with no enabled actions is a deadlock when any started
  thread is unfinished, including a thread blocked on `Join(target)` where
  `target` has not started and no remaining enabled spawn can start it.
