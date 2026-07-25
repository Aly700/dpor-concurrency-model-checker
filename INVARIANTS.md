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
- Fingerprint history is one path-local map shared by recursive DFS frames.
  Every fingerprint inserted for a child must be erased exactly once on every
  return path, including cycle cuts, schedule caps, and exceptions. Debug
  restore sampling compares the complete parent execution state and history
  with reference copies after the erase; sampling is a deterministic function
  of the numeric schedule, never time, randomness, or host scheduling.
- Cycle history may be omitted only when a static scan proves that the program
  has no `BranchNonzero` whose label-normalized target is its own action or an
  earlier action. In that case source transitions monotonically advance a
  normalized pc, TSO/PSO-only transitions strictly drain a finite buffer, and
  the first phase of `Wait` cannot return to its prior behavioral state without
  a signaling source transition that advances a pc. A non-last `BarrierWait`
  arrival likewise leaves its source pc unchanged, but strictly grows the
  fingerprinted arrival set and disables that participant. The set can return
  to empty only when a last arrival advances every participant past the
  barrier; without a backward branch none can return to that source pc.
  `TryLock` is an ordinary pc-advancing source transition on both success and
  failure, so adding it does not weaken this well-foundedness argument. A
  blocked `Upgrade` does not fire; every fired `Upgrade` or `Downgrade`,
  including modeled misuse, advances its normalized pc exactly once.
  Therefore a complete behavioral state cannot recur. Any future action that
  can decrease a pc or repeatedly mutate behavioral state without advancing a
  pc must extend or invalidate this proof before fingerprint elision is allowed
  (adr/0023, adr/0024, adr/0025, adr/0028).
- The behavioral state includes normalized per-thread PCs, startedness, wait
  phases, registers, memory values, mutex owners, reader-writer lock holders,
  nonzero semaphore permit counts, every nonempty barrier's configured party
  count and sorted current-generation arrival set, condition-variable wait
  sets, and ordered TSO store buffers or canonical per-address PSO FIFO maps.
  Vector clocks, absolute barrier generation ordinals,
  atomic/mutex/rwlock/semaphore/barrier clocks, race metadata, step counters,
  and schedule history are
  analysis/budget/history state and are excluded. Consequently a
  lasso proves schedule-existence of non-termination only, not repetition of
  analysis instrumentation. `TryLock` needs no new fingerprint field: its
  result is already present in the destination register, successful ownership
  is already present in `mutex_owner`, and its pc always advances. Rwlock
  conversions likewise need no new field: their observable mode is already
  present in the canonical reader set and writer owner, and a successful or
  erroneous conversion advances pc.
- A cycle witness is `stem + one cycle`, split at the first occurrence of the
  revisited state. The claim is existential. Its fairness field classifies only
  that witness and makes no system-level scheduler-fairness,
  starvation-freedom, or universal-liveness claim.
- Lasso fairness has three strongest-applicable witness classes. Cycle
  participants are the thread owners of all source and flush steps in the
  cycle. The legacy `unfair-schedule witness` predicate is unchanged: some
  non-participant thread has at least one enabled source or flush step at every
  replayed cycle state. Otherwise, a witness is a
  `strongly-unfair-schedule witness` when an exact non-participant endpoint is
  enabled at any cycle state, because repetition enables it infinitely often;
  it is `fair divergence` only when no non-participant endpoint is enabled
  anywhere in the cycle. Exact endpoint identity is
  `(thread, action_index[, PSO flush address])`. TSO and PSO pending flushes
  count, and a flush executed in the cycle makes its owner a participant.
- Fairness classification deliberately excludes every endpoint owned by a
  thread that takes any cycle step. It classifies this witness schedule under
  inter-thread scheduler fairness, not action fairness within a participating
  thread, and makes no program-level liveness claim.
- `fair_cycles + strongly_unfair_cycles + unfair_cycles == cycles_detected`.
  The first non-termination report remains first-found; its class does not
  imply the other classes are absent elsewhere in the explored space.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes,
  plus a numeric canonical address ID on PSO flush steps only.
- Replaying a schedule must produce the same state, report, and trace.
- One `BarrierWait` arrival is one replay step. A non-last participant remains
  at that action index but is disabled; the last arrival advances every parked
  participant atomically. Repeating a parked participant's endpoint before a
  release is invalid, and cyclic reuse of an endpoint is distinguished
  internally by the barrier generation even though the numeric schedule format
  remains unchanged.
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
- `TryLock(m) -> rN` always advances. On success it writes exactly `1`, becomes
  the mutex owner, and joins the acquiring thread with `mutex_clock[m]` exactly
  as `Lock` does. If `m` is held by any thread, including the caller, it writes
  exactly `0`, acquires nothing, and must not join either the stored mutex
  release clock or the live holder's clock. Failure never blocks and never
  changes ownership.
- A reader-writer lock has a last-writer release clock and an accumulated join
  of reader-release clocks. `RLock` joins only the writer clock; `RUnlock`
  accumulates its post-tick clock; `WLock` joins both clocks and then clears the
  reader accumulator; `WUnlock` replaces the writer clock. A successful
  `Upgrade` requires the caller's retained read hold and no other reader,
  atomically changes that hold to write mode, joins the same two frontiers as
  `WLock`, and clears the consumed reader accumulator. The caller's retained
  hold is never released into that accumulator and therefore creates no
  self-edge. `Downgrade` atomically changes the caller's write hold to read
  mode, publishes its post-tick clock as the writer release, and does not clear
  the reader accumulator; the retained reader's eventual `RUnlock` contributes
  normally. The accumulator is not cleared merely when the live reader count
  reaches zero. This protects writer publication and every
  reader-to-later-writer edge without creating a reader-to-reader edge that
  could hide a race.
- Every semaphore starts with zero permits. `SemPost` increments without a
  modeled ceiling and release-joins its post-tick clock into a lifetime
  accumulator; `SemWait` is enabled only at a positive count, decrements once,
  and acquire-joins that accumulator without clearing or replacing it. This is
  the documented strong-semaphore model: a waiter can be ordered after posts
  whose anonymous permits it did not consume, and safe verdicts are relative
  to that stronger model (adr/0022).
- For one barrier generation with participant post-tick clocks `C_i`, the
  release clock is exactly `R_g = join_i(C_i)`. Before any participant advances
  past that `BarrierWait`, its thread clock joins `R_g`. The arrival set and
  accumulator are then cleared and the generation increments. No prior-
  generation accumulator survives the reset: cross-generation HB exists only
  when an actual participant carries its joined thread clock into a later
  generation. `parties == 1` applies this rule immediately (adr/0024).
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
- Under TSO and PSO, `BarrierWait` is also an ordered point: an arrival remains
  disabled until all buffers owned by that thread are empty and performs no
  hidden flush.
- Under TSO and PSO, `TryLock` is a full ordered point like `Lock`: the source
  transition remains disabled until every buffer owned by the caller is empty,
  and neither success nor failure performs a hidden flush.
- Under TSO and PSO, `Upgrade` and `Downgrade` are full ordered points like the
  other rwlock operations. A conversion remains disabled until every buffer
  owned by the caller is empty and performs no hidden flush.
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
- The public action-only relation keeps every same-rwlock pair dependent,
  including `RLock(m)`/`RLock(m)`. With `Upgrade(m)` in the program, whichever
  root read acquisition runs first can expose its own sole-reader Upgrade
  before the other acquisition; after both reads neither Upgrade is enabled.
  The checker restores exact cross-thread `RLock(m)` commutation only when the
  complete program has no `Upgrade` on that rwlock name. It may commute the
  broader reader-mode pairs only when the program also has no `WLock`,
  `WUnlock`, or `Downgrade` on that name. These per-name static exclusions
  remove the conversion and last-reader/third-writer middle witnesses.
  Reentrant error endpoints remain protected by the terminal all-siblings
  backtrack safeguard; all conversion pairs and every other same-name pair
  remain dependent (adr/0021, adr/0028).
- Cross-thread, co-enabled `SemPost(s)` actions are independent: count addition
  and accumulated release-clock joins commute, neither poster acquires the
  other, and either first post enables the same waiter set. Every same-name
  pair involving `SemWait` remains dependent. A zero-permit wait uses the
  all-enabled disabled-transition repair so both alternate-poster middle-wait
  classes survive; selecting only one observed poster is forbidden
  (adr/0022). Different semaphore names are independent.
- The public action-only relation keeps every same-mutex pair involving
  `TryLock` dependent. Checker-local DPOR may commute two exact co-enabled
  `TryLock(m)` occurrences from different threads only when the node's
  snapshotted owner of `m` is a third thread distinct from both. Both attempts
  then fail and write `0` into disjoint thread-local registers; either order
  leaves identical PCs, registers, ownership, clocks, shared/race state, and
  enabled transitions. A free mutex, either trier as owner, a different
  same-mutex action, or a nonmatching endpoint remains dependent. Existing
  persistent closure retains intervening same-mutex operations; sleep
  inheritance evaluates the parent snapshot and keeps only the identical
  surviving endpoint; `TryLock` is never mutex-disabled, so it needs no new
  disabled-transition repair; and its pc advance supplies occurrence identity
  without a generation stamp. Different mutex names commute under the ordinary
  cross-cutting safeguards (adr/0025).
- The public action-only relation keeps same-name `BarrierWait` actions
  dependent because it cannot observe generation state. Checker-local DPOR may
  commute two valid co-enabled arrivals on the same barrier only when their
  stamped generation matches the node and `arrived_count + 2 < parties`.
  Both orders then produce the same sorted arrivals, accumulated clock, PCs,
  values, race state, and enabled set. Equality is dependent because the second
  arrival releases the generation. Every co-enabled same-generation sibling is
  nevertheless an initial persistent choice; sleep inheritance evaluates the
  relation at the parent count and retains only the identical child occurrence.
  A last arrival is dependent with the later action of every parked participant
  it releases. An incomplete barrier has no unique enabler and uses the
  all-enabled disabled-transition repair. Different barrier names commute when
  co-enabled (adr/0024).
- Assertion failures are first-class terminal reports with replayable,
  minimized schedules. They must not be downgraded to modeled errors.
- A barrier name has one program-wide positive party count. The strict CLI
  rejects zero or disagreement while loading; the direct `Program` API reaches
  the invalid `BarrierWait` as a deterministic forward modeled error so its
  endpoint and schedule remain replayable.
- Deadlock detection must distinguish a true cycle from a voluntarily finished
  thread, and must identify whether each unfinished disabled thread is waiting
  on a mutex, a join target, a condition variable, an rwlock writer, an rwlock
  reader set, an rwlock upgrade, a zero-permit semaphore, or an incomplete
  barrier generation. A barrier blocker is rendered
  `barrier NAME waiting_on_barrier`. A write-lock attempt by one of the current
  readers is a readers-to-drain deadlock with `self_wait`, not a modeled
  reentrancy error. A valid Upgrade blocked by other readers is rendered
  `rwlock NAME upgrade_waiting_for_readers_to_drain`; its retained own hold is
  not labeled as its blocker. Two readers that both Upgrade therefore form a
  deterministic two-thread deadlock.
- `TryLock` never appears in a deadlock blocker set. A backward branch that
  retries a failed attempt is a lasso/non-termination question. Under weak
  fairness, its witness is unfair when a nonparticipant holder's `Unlock`
  remains enabled at every cycle state; it is not reclassified as deadlock.
- Reader-writer-lock ownership is a reader-holder set plus an optional writer;
  non-holder or wrong-mode unlock, Upgrade, or Downgrade and read/write
  reentrancy are modeled errors. A valid Upgrade waits only for other readers;
  a valid Downgrade never blocks. Both conversions change the existing hold
  atomically, with no state in which the resource is unheld.
  `TryLock`, `Lock`, `Unlock`, and the mutex operand of `Wait` share the same
  mutex namespace and ownership state; `Unlock` validation is unchanged.
  A barrier resource name may not also be interpreted as a mutex (including a
  Wait mutex), rwlock, semaphore, or condition variable. Mutex, rwlock, and
  semaphore names retain their established pairwise separation. These distinct
  domains protect deterministic ownership, permit, generation, and HB
  semantics.
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
- Complete naive/DPOR gates compare fair-cycle, strongly-unfair-cycle, and
  unfair-cycle existence independently. Raw per-class cycle counts may differ
  because DPOR explores representatives; a missing class is a soundness
  failure, not an expectation to weaken silently.
