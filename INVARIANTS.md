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
  phases, registers, memory values, mutex owners, nonzero semaphore permit
  counts, condition-variable wait sets, and ordered TSO store buffers or
  canonical per-address PSO FIFO maps. Vector clocks, atomic/mutex/semaphore
  clocks, race metadata, step counters, and schedule history are
  analysis/budget/history state and are excluded. Consequently a
  lasso proves schedule-existence of non-termination only, not repetition of
  analysis instrumentation.
- A cycle witness is `stem + one cycle`, split at the first occurrence of the
  revisited state. The claim is existential. Its fairness field classifies only
  that witness and makes no system-level scheduler-fairness,
  starvation-freedom, or universal-liveness claim.
- Lasso fairness uses weak fairness. Cycle participants are the owners of all
  source and flush transitions in the cycle. A witness is an
  `unfair-schedule witness` exactly when some non-participant has at least one
  enabled source or flush transition at every replayed cycle state; otherwise
  it is `fair divergence`. Enabled at only some states is insufficient under
  weak fairness. TSO and PSO pending flushes count as enabledness, and a flush
  executed in the cycle makes its owner a participant.
- `fair_cycles + unfair_cycles == cycles_detected`. The first non-termination
  report remains first-found; its class does not imply the other class is absent
  elsewhere in the explored space.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes,
  plus a numeric canonical address ID on PSO flush steps only.
- Replaying a schedule must produce the same state, report, and trace.
- Labels are pseudo-actions and are never scheduled; replay validates each
  step against the normalized executable pc after skipping labels.
- Under TSO, an internal flush is scheduled as the executing thread with the
  reserved action index `kFlushActionIndex`. Replay must reject that sentinel
  unless the selected memory model is TSO and the thread's store buffer is
  nonempty at that exact step.
- Under PSO, every nonempty `(thread, address)` FIFO is a distinct enabled
  flush transition. It uses `kFlushActionIndex` plus that program address's
  canonical numeric ID. Replay must require the ID and reject the step unless
  that exact address FIFO is nonempty; SC/TSO source schedules remain unchanged.
- Replaying a non-termination witness must reproduce the identical stem/cycle
  report, including its fairness field, by exact equality between the
  end-of-stem and end-of-cycle behavioral states and by recomputing enabledness
  at every cycle state. Replay rejects schedules that continue after the cycle
  closes.
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
- A reader-writer lock has a last-writer release clock and an accumulated join
  of reader-release clocks. `RLock` joins only the writer clock; `RUnlock`
  accumulates its post-tick clock; `WLock` joins both clocks and then clears the
  reader accumulator; `WUnlock` replaces the writer clock. The accumulator is
  not cleared merely when the live reader count reaches zero. This protects
  writer publication and every reader-to-later-writer edge without creating a
  reader-to-reader edge that could hide a race.
- Every semaphore starts with zero permits. `SemPost` increments without a
  modeled ceiling and release-joins its post-tick clock into a lifetime
  accumulator; `SemWait` is enabled only at a positive count, decrements once,
  and acquire-joins that accumulator without clearing or replacing it. This is
  the documented strong-semaphore model: a waiter can be ordered after posts
  whose anonymous permits it did not consume, and safe verdicts are relative
  to that stronger model (adr/0022).
- `Signal(cv)` and `Broadcast(cv)` must add a happens-before edge from the
  signaling thread to every waiter they wake. They must not queue permits when
  no waiter exists.
- Conflicting memory accesses unordered by happens-before are races.
- Values are deterministic schedule-order int64 cell values. Plain reads have
  no weak-memory value semantics: in racy programs they observe the value
  produced by the explored interleaving, and the race report is the bug.
- Under TSO and PSO, a plain write's global visibility point is its flush, not
  its enqueue. Race metadata for the write must be recorded at the flush
  endpoint using the flushing thread's clock at that step. A buffered-model
  read forwards from the newest same-address entry in its own buffer before
  shared memory; forwarded reads are still recorded as plain reads
  conservatively so same-address dependence remains visible to DPOR.
- PSO has one FIFO per `(thread, address)`: same-address stores never reorder,
  while all pending different-address flushes are simultaneously schedulable.
  Atomics, CAS, synchronization operations, spawn/join, and `fence` remain
  disabled until every address FIFO owned by that thread is empty.
- Under TSO and PSO, both semaphore actions are ordered points and remain
  disabled until every buffer owned by the executing thread is empty. They do
  not perform hidden flushes.
- Atomic/atomic accesses are never races. Mixed plain/atomic same-address
  accesses are races when unordered by happens-before and at least one side is
  write-like: plain write, atomic store, successful CAS, or atomic RMW. CAS is
  still dependent as an atomic RMW regardless of runtime success.
- Register-only actions (`set`, `bnz`, `assert`) are thread-local and may be
  independent of every other thread's transition; same-thread program order is
  still never commuted. PSO internal flushes are not source-program actions:
  different-address flushes of one thread commute, but every co-enabled address
  must remain an explicit persistent-set choice so another thread can run
  between the two drains.
- Under TSO/PSO a source `Write` is a private enqueue: it touches only its
  owner's pc, clock, and store buffer, so it is independent of every other
  thread's transition (Spawn and Join keep their conservative edges). The
  later `Flush` is the globally visible write and must retain all same-address
  dependencies and race bookkeeping. Under SC, and for all same-thread pairs,
  `Write` dependence is unchanged (adr/0020).
- Cross-thread, co-enabled `RLock(m)` actions are classified independent. For
  successful acquisitions, both orders leave the same reader set, clocks,
  shared/race state, and enabled set, and a same-lock writer is disabled after
  either first reader and after both. Reentrant error endpoints are protected
  by the terminal all-siblings backtrack safeguard. Every other same-rwlock
  action pair is conservatively dependent at the public relation. The checker
  may commute all cross-thread reader-mode operations only when the complete
  program contains no writer-mode operation on that name; this static
  condition removes the last-reader/third-writer middle witness that would
  otherwise make the refinement unsound (adr/0021).
- Cross-thread, co-enabled `SemPost(s)` actions are independent: count addition
  and accumulated release-clock joins commute, neither poster acquires the
  other, and either first post enables the same waiter set. Every same-name
  pair involving `SemWait` remains dependent. A zero-permit wait uses the
  all-enabled disabled-transition repair so both alternate-poster middle-wait
  classes survive; selecting only one observed poster is forbidden
  (adr/0022). Different semaphore names are independent.
- Assertion failures are first-class terminal reports with replayable,
  minimized schedules. They must not be downgraded to modeled errors.
- Deadlock detection must distinguish a true cycle from a voluntarily finished
  thread, and must identify whether each unfinished disabled thread is waiting
  on a mutex, a join target, a condition variable, an rwlock writer, an rwlock
  reader set, or a zero-permit semaphore. A write-lock attempt by one of the
  current readers is a readers-to-drain deadlock with `self_wait`, not a
  modeled reentrancy error.
- Reader-writer-lock ownership is a reader-holder set plus an optional writer;
  non-holder or wrong-mode unlock and read/write reentrancy are modeled errors.
  A resource name may not be interpreted as more than one of mutex (including
  a Wait mutex), rwlock, and semaphore, protecting deterministic ownership,
  permit, and HB semantics.
- Thread completion is started-aware: a not-started static thread body is not
  finished for `Join` enabledness, even if the body is empty, but unstarted
  and unjoined bodies do not by themselves make clean termination a deadlock.
- A terminal state with no enabled actions is a deadlock when any started
  thread is unfinished, including a thread blocked on `Join(target)` where
  `target` has not started and no remaining enabled spawn can start it.
- Under TSO or PSO, a started thread with pc done but a nonempty store buffer
  is not finished. The nonempty buffer always enables a flush transition, so
  buffered writes alone do not create deadlocks.
- Cross-model bug existence is monotone for complete bounded explorations:
  `SC => TSO => PSO` independently for race, deadlock, error, assertion, and
  nontermination. Inclusion comparisons must skip any program for which a
  model is schedule-capped or retains a bound-exceeded execution.
- Cycle cutting is a terminal execution outcome like the step bound. DPOR must
  conservatively retain enabled siblings and must not sleep the cycle-closing
  transition across a sibling whose swapped prefix was not explored beyond the
  cut. Differential gates compare naive/DPOR cycle existence as a boolean;
  cycle counts may differ because DPOR explores class representatives.
- Complete naive/DPOR gates compare fair-cycle and unfair-cycle existence
  independently. Raw per-class cycle counts may differ because DPOR explores
  representatives; a missing class is a soundness failure, not an expectation
  to weaken silently.
