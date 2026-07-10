# Invariants — DPOR Model Checker

## Soundness

- The checker may explore too many schedules, but must not skip a schedule class that can expose a distinct bug.
- Independence may only be claimed for actions whose ordering cannot affect future enabledness or observable state.
- A report of safe means no explored state and no pruned equivalent class contains a bug under the modeled semantics.
- With backward branches, safety is relative to the configured per-thread step
  bound for executions that do not repeat a behavioral state. A result with no
  race/deadlock/error/assertion/non-termination but a nonzero
  `bound_exceeded_executions` count is clean only up to that bound, never
  unconditionally clean.
- A non-termination report must come from exact equality of canonical complete
  behavioral states on one execution path. A lossy hash is forbidden because a
  collision would fabricate a false divergence proof. Fingerprint history is
  reset on backtrack and must never deduplicate states across executions.
- The behavioral state includes normalized per-thread PCs, startedness, wait
  phases, registers, memory values, mutex owners, condition-variable wait sets,
  and ordered TSO store buffers. Vector clocks, atomic/mutex clocks, race
  metadata, step counters, and schedule history are analysis/budget/history
  state and are excluded. Consequently a lasso proves schedule-existence of
  non-termination only, not repetition of analysis instrumentation.
- A cycle witness is `stem + one cycle`, split at the first occurrence of the
  revisited state. The claim is existential and makes no scheduler-fairness or
  starvation-freedom claim.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes.
- Replaying a schedule must produce the same state, report, and trace.
- Labels are pseudo-actions and are never scheduled; replay validates each
  step against the normalized executable pc after skipping labels.
- Under TSO, an internal flush is scheduled as the executing thread with the
  reserved action index `kFlushActionIndex`. Replay must reject that sentinel
  unless the selected memory model is TSO and the thread's store buffer is
  nonempty at that exact step.
- Replaying a non-termination witness must reproduce the identical stem/cycle
  report by exact equality between the end-of-stem and end-of-cycle behavioral
  states. Replay rejects schedules that continue after the cycle closes.
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
- Under TSO, a plain write's global visibility point is its flush, not its
  enqueue. Race metadata for the write must be recorded at the flush endpoint
  using the flushing thread's clock at that step. A TSO read forwards from the
  newest same-address entry in its own buffer before shared memory; forwarded
  reads are still recorded as plain reads conservatively so same-address
  dependence remains visible to DPOR.
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
- Under TSO, a started thread with pc done but a nonempty store buffer is not
  finished. The nonempty buffer always enables a flush transition, so buffered
  writes alone do not create deadlocks.
- Cycle cutting is a terminal execution outcome like the step bound. DPOR must
  conservatively retain enabled siblings and must not sleep the cycle-closing
  transition across a sibling whose swapped prefix was not explored beyond the
  cut. Differential gates compare naive/DPOR cycle existence as a boolean;
  cycle counts may differ because DPOR explores class representatives.
