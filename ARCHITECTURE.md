# Architecture — DPOR Model Checker

## Pipeline

```text
.dpor text program -> CLI parser -> model::Program -> scheduler -> explored schedules -> HB/race/deadlock analysis -> minimal replay -> CLI report/replay schedule
                                                        \-> read-only showcase observations -> deterministic tree/evidence export
```

## Current scaffold

- `model::Program` is a tiny action IR with int64 values, eight
  thread-local registers per thread, label/branch control flow, and shared
  plain/atomic value cells.
- `dpor` is a deterministic CLI consumer that parses `.dpor` text programs,
  runs check or replay through the public `ModelChecker` API, and renders
  stable reports plus replayable schedule blocks.
- `ModelChecker::explore_naive` is the exhaustive oracle.
- `ModelChecker::explore_dpor` is the reduced oracle added beside the naive
  DFS. It uses deterministic backtrack/done sets, the public action-level
  `independent()` predicate, and checker-local transition refinements for
  per-name reader-writer-lock static guards, enabled valid `Join` operations,
  failed same-mutex `TryLock` pairs under a third-party owner, and
  generation-stamped barrier arrivals. Exact TimedWait phase/episode and
  Signal/Broadcast wake-target occurrences additionally protect cyclic
  historical transition matching.
- `ModelChecker::replay` re-executes a deterministic schedule under the same
  step bound and rejects disabled, out-of-range, or post-terminal steps with a
  clear error.
- `ModelChecker::minimize_schedule` greedily deletes per-thread tail steps only
  when replay proves the same race, deadlock, modeled-error, or assertion
  identity still reproduces; public exploration reports are normalized through
  it before return.
- `VectorClock` is the base happens-before data structure.
- `ModelChecker::collect_naive_schedules`, `collect_dpor_schedules`, and
  `inspect_schedule` are read-only observation APIs for verification and
  evidence tooling. They reuse the exact scheduler and replay interpreter;
  ordinary exploration passes no collector and pays no evidence-allocation
  cost.
- The cross-validation executable `dpor_oracle` checks a deterministic capped
  family of tiny programs against the naive oracle.

## Showcase observation and export boundary

The `showcase/` layer turns complete bounded checker runs into portfolio data
without becoming another semantics implementation. `collect_dpor_schedules`
threads an optional terminal-schedule sink through DPOR. The ordinary
`explore_dpor` call passes `nullptr`; the collection call records the exact
numeric representative whenever the existing result count records a normal
leaf, terminal error/assertion, exact cycle cut, or step-bound attempt. A
bound attempt is retained as a numeric terminal edge but is explicitly marked
`executed: false` by `inspect_schedule`.

`inspect_schedule` validates and advances through the same replay functions as
the CLI. For each step it exposes the sorted enabled endpoints, effective
action, executing thread's dense pre/post vector clock, and deterministic
register/shared-memory/store-buffer diffs. The observations are copied after
the scheduler decision; they cannot feed back into enabledness, HB, race
detection, or DPOR.

The export comparison expands every DPOR-reached numeric prefix. At an enabled
child prefix absent from the complete naive terminal set's DPOR subset, it
writes one `PRUNED` boundary with the exact number of naive schedules below
that prefix. These disjoint counts conserve
`dpor_explored + pruned == naive_equivalent`. This is an evidence visualization
of two actual complete explorations, not the independently checkable pruning
certificate deferred by ADR 0031: it does not claim to prove each local
independence decision.

`showcase/manifest.json` labels selected source/configuration bytes `CURATED`,
deterministic checker products `MEASURED`, and the stats ledger
`MEASURED_ENVIRONMENT_DEPENDENT`. Wall-clock microseconds are taken only in the
export process, never in core logic, and are the sole fields excluded from
regeneration comparison. Trees, witnesses, vector clocks, verdicts, schedule
counts, state-prefix counts, and depths remain byte-deterministic.

## Phase 1 execution model

The checker interprets a program as a small-step state machine:

- `pc[tid]` is the next executable action for each modeled thread, normalized
  past label pseudo-actions.
- `registers[tid][0..7]` are thread-local int64 registers initialized to zero.
- `thread_steps[tid]` counts scheduled steps for the per-thread execution
  bound.
- `mutex_owner[mutex]` records the owning thread for held mutexes acquired by
  either `Lock` or a successful `TryLock`.
- `mutex_clock[mutex]` records the vector clock stored by the last successful
  unlock of that mutex.
- `rwlocks[name]` records a canonical reader-holder set, an optional writer,
  the last writer-release clock, and the accumulated reader-release clock for
  that reader epoch.
- `semaphores[name]` records a zero-initialized unbounded modeled permit count
  and the lifetime componentwise join of every post-release clock. Zero-count
  entries are behaviorally equivalent to absence.
- `barriers[name]` records the canonical program-wide party count (positive in
  a valid program), a private generation ordinal, the sorted set of
  participants parked in the current generation, and the componentwise join
  of those participants' arrival clocks. The arrival set is behavioral; the
  ordinal and clock are analysis bookkeeping.
- `atomic_location_clock[address]` records the per-address release sequence for
  modeled atomic operations. The model supports acquire loads, release stores,
  acquire-release RMWs, and SC-per-location; it does not model relaxed atomics.
- `condition_waiters[cv]` records sleeping waiters in deterministic thread-id
  order. Signal wakes the lowest-numbered parked waiter; Broadcast atomically
  snapshots and wakes the complete current set. TimedWait timeout removes its
  own exact parked episode. Neither wake action queues a permit when the set is
  empty.
- `thread_clock[tid]` records each thread's happens-before frontier.
- `started[tid]` records whether a static thread body is alive in the current
  execution. Threads targeted by valid `Spawn` actions start disabled and
  become schedulable only after a successful spawn.
- `wait_phase[tid]` records whether a thread is not waiting, asleep in a
  condition variable, or resolved by wake/timeout and waiting to reacquire its
  mutex.
- `memory[address]` records the last plain write, the plain reads since that
  write, and deterministic plain/atomic access lists used only for the mixed
  plain/atomic race check.
- `memory_values[address]` records the current int64 value for both plain and
  atomic accesses in the shared address namespace.
- Under `MemoryModel::TSO`, `store_buffers[tid]` is a FIFO of pending plain
  writes. A source `Write` enqueues; an internal flush transition commits the
  oldest entry. Under `MemoryModel::PSO`, `pso_store_buffers[tid]` is a
  canonical address map of FIFOs and each nonempty address is a distinct flush
  choice. `MemoryModel::SC` has no pending store buffers and remains the default.

At each DFS state, the naive oracle enumerates exactly the enabled transitions
in deterministic schedule-step order. Not-started threads are disabled. `Read`, `Write`,
`AtomicLoad`, `AtomicStore`, `AtomicRmw`, `CompareExchange`, `Set`,
`BranchNonzero`, `Assert`, `Fence`, `Yield`, `TryLock`, `Unlock`, `RUnlock`,
`WUnlock`, `Downgrade`, `SemPost`, `Spawn`, `Signal`, and
`Broadcast` are enabled for started unfinished threads; invalid `Unlock`,
invalid rwlock unlock/conversion/reentrancy, invalid `Wait`, invalid `Spawn`,
and invalid `Join` steps are reported as modeled errors. `Lock` is enabled only when its
mutex is not currently owned; `TryLock` remains enabled regardless of the
owner and reports failure through its register result. `RLock` is enabled in
the absence of another thread's writer; `WLock` is enabled only with no writer
and no readers, except that a current writer's recursive acquisition remains
executable as an error.
A valid `Upgrade` caller retains its read hold and becomes enabled only when it
is the sole reader. Invalid ownership remains executable as a modeled error.
`Downgrade` never blocks; invalid write ownership likewise reaches a modeled
error.
A read holder's attempted `WLock` stays disabled on its own hold and therefore
forms a tagged self-wait deadlock.
`SemWait(name)` is enabled exactly when the named semaphore has a positive
permit count; it consumes one permit. `SemPost(name)` always increments and
queues a permit even when no waiter exists. There is no declaration or hidden
initial permit.
`BarrierWait(name, parties)` records one arrival. A non-last arrival remains at
the same source pc and is disabled until the generation completes. The last of
`parties` distinct participants atomically releases the complete arrival set,
advances every participant past its wait, clears the generation state, and
increments the private generation ordinal. `parties == 1` therefore releases
immediately. One arrival is one schedule step; release is not a second
transition. Every use of a barrier name must specify the same positive party
count.
`Join(target)` is enabled only when `target` has started and finished.
`Wait(cv, mutex)` is one IR action with two schedule steps at the same action
index: release-and-sleep, then after wakeup reacquire-and-resume.
`TimedWait(cv, mutex, rN)` adds an explicit parked timeout step at that same
index. While parked, timeout is permanently enabled; it writes `0` and enters
reacquisition, while Signal/Broadcast wake writes `1`. Reacquisition remains
the existing effective Lock step and is the only one that advances the waiter
pc.
A state with started unfinished threads and no enabled action is a deadlock;
the report tags each started blocked thread as waiting on a mutex, join target,
plain condition variable, rwlock writer, rwlock reader set, rwlock upgrade,
semaphore, or incomplete barrier generation. A parked TimedWait cannot appear
in such a terminal state because timeout is enabled; after resolution, its
mutex reacquisition can be a normal mutex blocker. Upgrade waits use the distinct
`rwlock NAME upgrade_waiting_for_readers_to_drain` blocker. A state where all
started threads are finished is normal
termination, even if some static thread bodies were never spawned. If an
enabled choice names a thread that has already executed its per-thread step
bound, that execution terminates with a bound outcome and increments
`bound_exceeded_executions`.

Under TSO and PSO, atomics, CAS, mutex/condition-variable operations (including
`TryLock`), all six rwlock operations, both semaphore operations, barrier
waits, spawn, join, and `Fence` are ordered points: they are disabled until all
of the executing thread's buffers are empty. A nonempty buffer always enables
a flush for that thread, including after the source pc is done, so buffered
completion is not deadlock. In particular, `Wait`, `Signal`, and `Broadcast`
wait for explicit drains and never flush a buffer internally. TimedWait uses
the same ordered-point rule: park waits for explicit drains, timeout performs
no hidden drain, and reacquisition is an effective Lock.

## Mesa Condition Variables

The first phase of `Wait(cv, mutex)` atomically releases the caller's mutex and
adds the caller to the sorted parked set for `cv`, while retaining the same
source endpoint. `Signal(cv)` removes and wakes the lowest-numbered parked
thread. `Broadcast(cv)` snapshots and removes the complete sorted parked set
at its firing point and moves each member to `wait_phase = reacquiring`.
Neither wake action grants mutex ownership: every receiver later executes the
existing second Wait phase and contends through ordinary mutex enabledness and
dependence before advancing its pc.

An empty Broadcast advances only the broadcaster's pc and clock. It stores no
condition permit, clock, or history, so a later Wait still parks and can
produce the ordinary condition-variable deadlock. The effective Broadcast
occurrence nevertheless records an engaged exact empty waking set for DPOR;
that analysis identity is distinct from a non-Broadcast's absent component.

`TimedWait(cv, mutex, rN)`, spelled
`timedwait CONDITION MUTEX -> rN`, uses the same release-and-park phase. While
parked, its own source endpoint remains enabled as a nondeterministic logical
timeout. Taking timeout removes exactly that parked episode, writes `0`, and
moves to mutex reacquisition. Signal or Broadcast may win instead, write `1`,
and move the waiter to that same phase. Neither resolution grants the mutex;
only the later effective Lock advances the waiter pc.

There is no duration, deadline, wall-clock query, step threshold, or hidden
random branch. Timeout is an explicit schedule choice. A timeout replay
therefore contains park, timeout, and reacquisition at one numeric endpoint; a
wake replay contains park and reacquisition with the wake action between them.
A lost wake remains a deadlock for plain Wait but is an explorable timeout
schedule for TimedWait.

Each TimedWait park/timeout carries exact phase-and-episode identity. Signal
and Broadcast carry exact internal wake targets as
`(thread, wait action_index, episode)` vectors, including engaged empty
vectors. This strengthens Broadcast's public thread-only waking-set diagnostic
without changing numeric schedules.

## Cyclic Barriers

`BarrierWait(name, parties)` has one program-wide positive party count per
name. The CLI rejects zero and disagreement during parsing. A directly
constructed `Program` instead preserves either invalid action as a forward
modeled error, so the endpoint and report remain replayable. Barrier names are
distinct from mutex, reader-writer-lock, semaphore, and condition-variable
names; parser and direct-API construction reject a collision in either order.

One scheduled arrival either parks at its unchanged source pc or, when it is
last, releases and advances the complete sorted participant set. The release is
part of that last arrival rather than a hidden second transition. Deadlock
reports tag every participant left in an incomplete generation as
`barrier NAME waiting_on_barrier`. Replay uses the ordinary numeric source
endpoint, while DPOR stamps its internal enabled and executed occurrences with
the generation ordinal so cyclic reuse cannot alias two generations.

## Try-Lock

`TryLock(m, rN)` has the strict text spelling `try_lock m -> rN` and uses the
same mutex resource, owner, release clock, and cross-namespace collision rules
as `Lock`, `Unlock`, and the mutex operand of `Wait`. It never waits. Every
execution advances the caller's pc and writes exactly one result: if `m` is
free, it writes `1` and becomes owner; if any thread (including the caller)
owns `m`, it writes `0` and changes no mutex state. Existing `Unlock`
owner validation applies unchanged after either outcome.

A failed attempt is therefore absent from deadlock blocker sets. A backward
branch may turn repeated failures into a lasso. That is reported and replayed
through the existing non-termination machinery; weak fairness classifies a
holder-starvation witness as unfair when the holder's `Unlock` remains enabled
throughout the cycle.

## Reader-Writer Lock Conversion

`Upgrade(name)` has strict text spelling `upgrade NAME`. The caller must hold
read mode on that rwlock. It retains that hold while disabled and can execute
only when no other reader remains, then atomically removes the read hold and
installs the caller as writer. `Downgrade(name)`, spelled `downgrade NAME`,
requires write mode, never blocks, and atomically replaces writer ownership
with a read hold. There is no intermediate unowned state and no reentrancy.

No-holder and wrong-mode conversions follow the established mismatched-unlock
convention: the action ticks and advances its pc, changes no ownership, and
reports `thread T attempted to upgrade|downgrade rwlock 'NAME' but it does not
hold that mode`. A blocked valid Upgrade does not execute and therefore cannot
mutate clocks or ownership.

Two threads that retain read mode and both reach Upgrade cannot make either
caller the sole reader. The ordinary terminal blocked-thread machinery reports
both with `BlockedOnKind::RwLockUpgrade`, the rwlock name, no owner, and no
`self_wait`. CLI check and numeric replay preserve that exact blocker set and
schedule.

## Happens-Before Analysis

Each executed step ticks its thread's vector clock. `Unlock` stores the
releasing thread's clock in the mutex clock. `Lock` joins the acquiring thread's
clock with that mutex clock. A successful `TryLock` performs exactly that same
join after becoming owner. A failed `TryLock` performs no join at all: neither
the stored release clock nor the current owner's live clock is acquired.
`Wait` first performs the same release update as
`Unlock`, then its woken second step performs the same acquire join as `Lock`.
TimedWait performs those same park and reacquisition updates. A wake writes
`1` and retains the following Signal/Broadcast edge; timeout writes `0` and
performs no condition-variable join in either direction. Its only later
ordering is the mutex-clock join at reacquisition.
`Signal` joins its post-tick clock into the selected waiter.
`Broadcast` snapshots every currently parked waiter and independently joins
the same broadcaster post-tick clock into each receiver. Waking one receiver
does not change the clock used for another, so Broadcast itself creates no
waiter-waiter edge. An empty snapshot performs no join and retains no clock
that a future Wait or Signal could acquire.
`RLock` joins only the named rwlock's last writer-release clock. `RUnlock`
joins its post-tick clock into the rwlock's reader-release accumulator.
`WLock` joins both the last writer release and every accumulated reader
release, then clears the reader accumulator for the next epoch. `WUnlock`
replaces the writer-release clock with its post-tick clock. A successful
`Upgrade` performs the same two joins and accumulator clear as `WLock` before
atomically entering write mode. Its retained own read hold has not been
published to the accumulator, so there is no self-edge. `Downgrade` publishes
its post-tick clock as the writer release while atomically retaining read
ownership, and leaves the reader accumulator unchanged; its later `RUnlock`
contributes in the ordinary way. In particular, readers never acquire one
another's release clocks.
`SemPost` joins its post-tick clock into the named semaphore's lifetime release
accumulator without acquiring it. A successful `SemWait` joins that entire
accumulator into the waiter and never clears it. This deterministic strong
model can order a waiter after posts whose anonymous permit it did not consume;
ADR 0022 makes that verification-model caveat explicit.
For barrier generation `g`, let `C_i` be participant `i`'s clock after its
arrival step ticks. The generation release clock is exactly

```text
R_g = join over all arrivals i in g of C_i
C_i := C_i join R_g                 for every released participant i
```

No participant advances past the wait before that join. Release then clears
both the arrival set and its accumulator before incrementing the generation,
so a later generation cannot acquire an earlier generation's clocks merely by
reusing the name. Cross-generation HB exists only through a real participant's
joined thread clock and program order into a later arrival. Fixed positive and
negative probes pin both arrival directions, reject leaked old-generation
edges, and retain genuine participant-carried chains.
`Spawn(target)` marks the target started and joins the target thread's clock
with the spawner's post-tick clock, so pre-spawn actions happen-before all
target actions.
`Join` joins the caller's clock with the target thread's final clock.
`AtomicStore(address)` replaces the address's atomic location clock with the
storing thread's post-tick clock and does not acquire from the old clock.
`AtomicLoad(address)` joins the loading thread with that location clock and
does not mutate it. `AtomicRmw(address)` joins the current location clock into
the executing thread, then replaces the location clock with the joined thread
clock. Store and RMW replacement is deliberate: joining old location clocks
would fabricate happens-before edges and can hide real plain-data races.
Successful `CompareExchange(address)` uses the same acquire-release
join-then-replace path as `AtomicRmw`. Failed compare-exchange is acquire load
only and must not replace the location clock.

Values ride on top of these clock rules. Plain reads and atomic loads copy the
current schedule-order cell value into a destination register when one is
present. Plain writes and atomic stores update the cell from an immediate or
register operand. Atomic RMW returns the old value and adds its operand. CAS
stores on success and writes `1` or `0` to its result register. `TryLock`
likewise writes `1` for acquisition and `0` for failure into its destination
register, so existing value flow and `BranchNonzero` directly implement retry
loops. TimedWait uses the same value path: `0` selects timeout/retry/fallback
logic and `1` selects wake logic.

Memory accesses compare their current vector clock against prior conflicting
accesses to the same address. Under TSO or PSO, an enqueued plain write records no
race metadata until its flush, which is the write's global visibility point.
Plain reads forward from the reading thread's newest same-address buffered
write before shared memory but are still recorded as plain reads
conservatively. Plain/plain races use the existing last-write and
reads-since-last-write metadata. Atomic/atomic accesses never race. Mixed
plain/atomic accesses race when unordered by happens-before and at least one
side is write-like: plain write, atomic store, successful CAS, or atomic RMW.
A race report records the two access endpoints and the executed prefix through
the second access. Assertion reports record the assertion endpoint, register,
observed value, and executed prefix.

The exhaustive oracle explores all enabled interleavings of the modeled
small-step semantics. Therefore, per-execution happens-before race detection
over every explored schedule is complete for this model: if a modeled race or
deadlock is reachable in any enabled interleaving, the naive oracle will visit a
prefix that reports it.

## Exploration State and Cost Control

Both explorers still copy `ExecutionState` for each executable child. ADR 0023
measured that copy traffic, but found that the larger multiplier was copying
the complete behavioral-fingerprint history alongside every child. DFS now
owns one history map per active path: a child inserts its exact fingerprint and
an RAII restore guard erases that precise insertion on return. Map iterators are
stable across descendant insertions and erasures, so nested guards restore the
same map value that a reference copy would have contained. A Debug-only,
deterministically sampled boundary assertion compares both the complete parent
`ExecutionState` and the restored history against reference copies.

Upgrade and Downgrade add no behavioral-fingerprint field. Their observable
mode is already represented by normalized pc plus the canonical rwlock reader
set and optional writer owner. A blocked Upgrade does not execute; every fired
conversion advances pc and changes ownership on success, while misuse advances
pc without changing ownership. The existing state therefore distinguishes all
program-observable conversion outcomes. Rwlock release clocks remain excluded
analysis state under the established lasso policy.

Broadcast likewise adds no behavioral-fingerprint field. Its exact waking-set
stamp is DPOR analysis metadata. A nonempty Broadcast changes the already
fingerprinted `condition_waiters` and per-thread Wait phases; every fired
Broadcast, including an empty one, advances normalized pc. The existing state
therefore distinguishes its program-observable outcomes while excluding wake
clocks and occurrence metadata.

TimedWait also adds no behavioral-fingerprint field. Fingerprints are compared
within one fixed Program, so normalized pc identifies whether its static action
is Wait or TimedWait. Existing waiter membership and Wait phase distinguish
park and resolution, the destination register distinguishes timeout `0` from
wake `1`, and mutex ownership plus pc distinguish reacquisition. Per-thread
step ordinals and exact episode/wake-target stamps remain analysis metadata;
including them would prevent a genuine timeout-spin state from recurring.

Programs without a normalized self/backward `BranchNonzero` do not allocate or
construct cycle history. Every ordinary source step advances a normalized pc;
`TryLock` does so on both outcomes, with its result already represented by the
existing register array and successful ownership by `mutex_owner`; a blocked
`Upgrade` does not fire, while every fired `Upgrade` and `Downgrade` advances
pc exactly once; every fired `Broadcast`, including an empty one, also advances
pc exactly once, and nonempty fan-out changes fingerprinted waiter state;
the exceptional first `Wait` phase changes the fingerprinted wait/ownership
state and needs a later pc-advancing Signal or Broadcast before it can resume;
a TimedWait park changes the same state, timeout then changes waiter membership
and phase at the unchanged pc, and its effective Lock changes phase/ownership
and advances pc; a wake instead requires the existing pc-advancing wake action
before that reacquisition;
a non-last
`BarrierWait` strictly grows the fingerprinted arrival set and disables that
participant until another participant's pc-advancing arrival releases the
generation; and TSO/PSO flush-only progress strictly drains finite buffers.
The release advances every parked participant, so without backward control
flow none can return to the same barrier pc. Those well-founded measures
exclude an exact repeated behavioral state. This classifier must be revisited
if the action set gains any other pc-decreasing or same-pc repeatable state
transition.

At a DPOR node, enabled schedule steps are collected once and that exact sorted
vector is reused to materialize the effective-transition map. Sleep-set
inheritance returns immediately when the parent sleep set is empty, since its
result is necessarily empty. These changes remove duplicate enabledness scans
without reconstructing or reordering a choice set.

Default-off profile targets (`DPOR_BUILD_EXPLORATION_PROFILE`) add deterministic
logical counters for state/history copy volume, fingerprints, clocks,
enabledness, transition maps, replay, and report bookkeeping. They are a serial
diagnostic facility, not part of normal exploration and not a source of timing
inside the core. Some counters are intentionally lower-bound proxies (for
example, gallery CLI child processes are separate), so wall-time claims come
only from uninstrumented Release harness runs.

## Lasso Fairness Classification

An exact cycle report replays `stem + cycle` once more and classifies that
witness without changing exploration. Cycle participants remain the thread
owners of every scheduled source or flush step, preserving ADR 0018. For each
non-participant, replay observes interpreter-enabled exact schedule endpoints
at the cycle start and after every cycle step. Source and TSO endpoints are
`(thread, action_index)`; PSO flush identity also carries the canonical address
ID.

The classifier retains the old weak predicate verbatim. If some
non-participant thread has an enabled endpoint at every state, the report is
`unfair-schedule witness`. Otherwise, a nonempty union of exact endpoints over
the cycle is `strongly-unfair-schedule witness`: the repeated cycle enables at
least one postponed endpoint infinitely often. An empty union is
`fair divergence`. The first applicable label wins, and every cycle increments
exactly one of `unfair_cycles`, `strongly_unfair_cycles`, or `fair_cycles`.

This is deliberately witness- and thread-scheduler-scoped. Endpoints owned by
a thread that takes any cycle step are excluded, so the field does not claim
action fairness within a participating thread or program-level liveness.
Classification runs only after exact cycle closure; acyclic and residual-bound
executions pay no added search or post-processing cost.

A single thread that repeatedly parks, chooses timeout, reacquires, and
re-parks owns every step of its exact cycle and is therefore `fair divergence`.
If a separate Signal endpoint remains enabled at every state but is never
scheduled, that nonparticipant makes the witnessed timeout spin an
`unfair-schedule witness`. The timeout choice owned by the participating waiter
is intentionally outside this inter-thread fairness field.

## DPOR Reduction

DPOR maintains a stack of prefix nodes with sorted enabled, backtrack, done, and
sleep schedule-step sets. Each enabled node records both the replay endpoint and the
phase-aware effective action for each enabled thread, so `Wait` release/sleep,
TimedWait park/timeout, and resolved mutex reacquire are reduced as different
transition semantics while source and TSO schedule steps remain
`(thread, action_index)` pairs. TimedWait park and timeout additionally carry
their exact per-thread episode; resolved reacquisition is an effective Lock.
Barrier
occurrences additionally carry the current generation ordinal internally,
because a cyclic program can execute the same `(thread, action_index)` in more
than one generation. Signal and Broadcast occurrences carry exact wake-target
vectors of `(thread, wait action_index, episode)`: engaged empty when they wake
nobody, one target for nonempty Signal, and every parked receiver for
Broadcast. The public Broadcast trace retains its sorted thread-only waking
set.

TSO and PSO flushes use the reserved action index `kFlushActionIndex` in those
same sets. TSO keeps the original two-number endpoint; PSO adds the canonical
numeric address ID to `ScheduleStep`, so several address flushes from one
thread are distinct enabled keys and replay selects the exact queue.

Different-address PSO flushes owned by one thread commute as direct state
updates, but they are not source-program ordered. The persistent-set
initializer conservatively inserts every co-enabled sibling address whenever
one such flush is selected. This prevents the checker's general same-thread
trace-order rule from collapsing the two scheduler choices and permits another
thread to observe state between them.

Dynamic backtracking follows the Flanagan-Godefroid last-point rule for enabled
transitions: it adds the later thread only at the last earlier dependent
transition that is not happens-before ordered before it. Each trace entry stores
the executing thread's post-step vector clock for this check. Enabled valid
`Join(target)` transitions use a checker-local transition-aware independence
rule: after the target is finished, the join commutes with unrelated non-spawn
transitions. Invalid joins, target-thread transitions, spawns, and joins that
wait for the joiner remain dependent.

If the later effective transition was not enabled at a dependent candidate
prefix, DPOR first tries an enabler-chain repair. Where a concrete chain can be
proved, it adds only enabled threads that can advance it: a remaining
`Spawn(t)` for a not-started thread, the target's remaining execution for
`Join(t)`, a same-thread prerequisite, a mutex or rwlock writer, the other
rwlock readers that must drain for `WLock` or `Upgrade`, or a
`Signal`/`Broadcast` chain for a sleeping waiter. Anonymous/cyclic/unknown
chains, including semaphore posts, barrier arrivals, and mutually blocked
upgraders, use ADR 0010's all-enabled repair. A top-level blocked `Lock` or
woken wait reacquire also retains that fallback. A parked TimedWait needs no
wake enabler repair because timeout is itself enabled; after resolution its
blocked effective Lock uses the ordinary mutex repair.

Godefroid sleep sets prune alternatives whose trace class has already been
represented. A child inherits slept threads only while their current effective
next action is still independent of the transition just executed; slept threads
are never executed, and explored choices are added to the node sleep set for
later alternatives. Modeled-error endpoints still clear pruning at that node and
add every enabled sibling, and sleep-blocked prefixes still apply the disabled
fallback before being pruned.

Same-condition `Wait`, `TimedWait`, `Signal`, and `Broadcast` remain pairwise
dependent; different condition names commute subject to the existing
cross-cutting rules. This deliberately keeps different waiters'
same-condition timeouts dependent: their adjacent final-state updates commute
locally, but no complete proof covers persistent closure, a middle wake,
historical matching, repair, and sleep inheritance. There is no empty-wake
exception.

Complete TimedWait episode and wake-target identity is compared at enabled and
executed records, node matching, backtracking and persistent closure, disabled
repair, checker-local independence, and sleep inheritance. Conservative
dependence already prevents a same-condition parked-set mutation from crossing
an inherited sleep edge. Exact stamps separately prevent a later cyclic park,
timeout, Signal, or Broadcast from falsely matching an earlier historical
occurrence at the same numeric endpoint. Resolved reacquisition then uses
ordinary same-mutex dependence. ADR 0030 records the mutation discriminator
and boundary-by-boundary audit; ADR 0029 remains the Broadcast fan-out
predecessor.

Race, error, and assertion detection still run through the same interpreter as
naive exploration, and every DPOR report is expected to replay identically.
Bound outcomes also run through that interpreter. Bound counts may differ
between naive and DPOR, so the gates compare only whether any execution hit the
bound.

Atomics do not add blocking or phase changes. `effective_next_action()` passes
them through unchanged, and sleep-set inheritance plus happens-before-aware
backtracking rely only on the updated `independent()` and `may_conflict()`
clauses. Two same-address atomic loads are independent; same-address pairs
involving atomic store or RMW are dependent; and same-address mixed
plain/atomic pairs are dependent.
CAS is dependent as an atomic RMW at the action level regardless of runtime
success. Register-only actions are independent of every other thread's
transition because they touch no shared state and do not affect cross-thread
enabledness; same-thread transitions are still never commuted.

Every same-rwlock pair is dependent in the public action-only relation. The
old unconditional `RLock`/`RLock` clause is unsound when the same name has an
Upgrade: after the first read acquisition that thread can be the sole reader
and execute Upgrade, but after both acquisitions neither conversion is
enabled. The middle Upgrade was not enabled at the root, so a persistent set
cannot recover the opposite upgrader-first class merely from the local
two-RLock state equality.

Checker-local DPOR restores cross-thread `RLock`/`RLock` independence only when
the complete program contains no Upgrade on that rwlock name. It restores the
broader reader-mode refinement only when the name has none of `WLock`,
`WUnlock`, `Upgrade`, or `Downgrade`. Both checks are per-name, so a conversion
on one rwlock cannot suppress safe commutation on another. Every same-name pair
involving a conversion remains dependent.

Upgrade-specific disabled repair follows the enabler heads of all *other*
current readers and skips the caller's retained hold. Unknown or cyclic
chains—including two readers blocked on each other's Upgrade—select the
existing all-enabled fallback. Legacy `WLock` while holding read mode retains
its permanent `self_wait` treatment. Persistent closure and sleep inheritance
therefore see every conversion as dependent, and a fired conversion's pc
advance provides occurrence identity without a new generation stamp.

Two cross-thread `SemPost` operations on one name commute because count
addition and vector-clock join are commutative and neither poster acquires the
other. Every same-name pair involving `SemWait` remains dependent. A wait that
was disabled at a zero-count prefix deliberately uses the all-enabled repair;
this retains both alternate-poster middle-wait traces when posts commute.
Different semaphore names use the ordinary disjoint-resource independence
rule.

The public action relation conservatively keeps every same-mutex pair
involving `TryLock` dependent. The checker-local exception applies only to two
different threads' exact co-enabled `TryLock(m)` occurrences when the node's
snapshotted `mutex_owner` says that a third thread owns `m`. Both attempts must
fail. Each order ticks and advances both triers and writes `0` to their
disjoint thread-local registers, while leaving ownership, the mutex release
clock, shared values, race metadata, and the resulting enabled set identical.

The surrounding DPOR safeguards remain part of that diamond. Free-mutex pairs,
an owner that is one of the triers, and every mixed same-mutex pair stay
dependent. Those dependencies give persistent closure around an intervening
`Unlock`, `Lock`, or `Wait`. Sleep inheritance consults the parent node's owner
snapshot and inherits only an endpoint/action occurrence that remains enabled
in the child. A source `TryLock` is never disabled by mutex ownership, so the
established buffered ordered-point and generic disabled-transition repairs need
no TryLock-specific path. It always advances its pc, so its numeric endpoint
supplies occurrence identity without the generation stamp required by cyclic
barriers. The exact three-thread discriminator has T2 acquire and retain `m`
while T0 and T1 each try once: naive explores four terminal orders, whereas
DPOR commutes only the two T2-first failures and explores exactly three.

Two valid, co-enabled arrivals on one barrier generation are independent only
at a node with current arrival count `k` satisfying `k + 2 < parties`. In that
case neither adjacent order releases the generation: both orders leave the
same two newly parked threads, sorted arrival set, joined accumulator, PCs,
clocks, values, race state, and enabled set. If `k + 2 == parties`, the second
arrival releases everyone and the identity of the last arriver affects the
effective transition, so the pair is dependent. The public action-only
`independent()` remains conservative for all same-name barrier pairs because it
cannot observe `k` or the generation; different names commute under the usual
cross-cutting safeguards.

That local diamond is not sufficient by itself. Initial persistent-set closure
keeps every co-enabled sibling arrival on the same generation, preserving a
third thread's opportunity to run between early arrivals and choose a different
last arriver or generation cohort. Sleep inheritance evaluates independence
from the parent snapshot and inherits an entry only if the identical
generation-stamped occurrence remains enabled in the child. A last arrival is
dependent with every parked participant action that it releases. Incomplete
barrier generations have no unique enabler and therefore use the all-enabled
disabled-transition repair at ordinary, terminal, and sleep-blocked prefixes.
The three-thread `parties == 3` discriminator has six naive arrival orders and
three DPOR representatives: the first two arrivals commute for each choice of
last arriver, while the three possible last arrivers are distinct dependent
classes.

Spawn adds dynamic enabledness. Not-started threads are absent from enabled
sets and sleep sets, and replay rejects target-thread steps before the
corresponding spawn. At terminal leaves, a not-started thread with a non-empty
body contributes its first action as a disabled transition so DPOR can repair
prefixes where a spawn could have enabled it. `independent()` treats every pair
involving `Spawn` as dependent because the action-only predicate cannot know
whether the other action belongs to the target.

The trace records which `Spawn` transitions actually started a thread. Disabled
repair for a later target-thread transition does not move before that
successful spawn enabler, while modeled-error spawn attempts are ignored as
enablers. Disabled-transition repair removes sleep entries for every inserted
repair choice; otherwise a dynamically added backtrack can be skipped solely
because it was previously slept.

## Verification Gates

The DPOR implementation is checked against deterministic gates. The 2-thread
oracle sweep enumerates small programs by length pair over a 28-action alphabet
and compares naive vs. DPOR race/deadlock/error/assertion existence, the
bound-hit boolean, total-cycle existence, all three fairness-class existence
booleans, schedule dominance, and report replay identity. The
3-thread oracle sweep uses an evenly strided deterministic sample of the larger
26-action, 6-slot space, plus hand-picked disabled-transition cases, to
exercise spawn, join, condition-variable, rwlock, semaphore, and barrier
enabledness. Fixed spin and mutex-blink lassos keep its weak, strong, and fair
class comparisons non-vacuous. A
separate three-reader fixture pins 1680 naive leaves and one DPOR
representative. The fixed-seed differential fuzz gate combines 3,000 random
2-5-thread, 1-6-action programs with deterministic one-thread TimedWait
coverage programs. The random lanes cover plain and atomic memory, mutexes,
rwlocks, semaphores, barriers, TryLock, TimedWait, condition variables, joins,
yields, spawn-shaped programs, and modeled-error cases, plus a value-mode lane
with registers, branches, CAS, fetch-add, assertions, and deliberate bound
hits. Capped explorations are counted but excluded from verdict equality
because truncation can legitimately hide a later endpoint. Fuzz compares all
three lasso classes and runs fixed uncapped mutex-blink and TimedWait outcome
discriminators even when the seeded corpus contains no such witness.

The TimedWait-widened acceptance run checked 22,903 two-thread programs
(64,582 naive versus 35,705 DPOR schedules), 65,547 three-thread programs
(770,547 naive versus 339,177 DPOR schedules), 11,877 TSO programs (54,022
versus 19,980), and 6,707 PSO programs (30,045 versus 10,882). Every oracle
observed fair, strongly-unfair, and weakly-unfair cycle existence under both
naive and DPOR exploration. Both buffered oracles
enforce zero capped skips. Their action alphabets are respectively 28, 26, 23,
and 23. The fixed fuzz run compares 3,369 of 3,392 programs and reports 23
capped programs; it generates 407 TimedWait actions and compares 406
(202/201 in the mostly-well-formed lane and 205/205 in the adversarial lane).
Fixed outcome probes observe both timeout `0` and wake `1`.
Cross-model inclusion compares all 1,747 programs and runs 17,470 checks with
zero global skips; its dedicated TimedWait corpus is 4 attempted / 4 compared
/ 0 skipped. The
unchanged optimality corpora retain byte-identical meter ratios SC 1.067, TSO
1.152, and PSO 1.154.

The optimality meter is the fourth gate. Its SC corpus collects all naive
maximal schedules for small non-error/non-assertion programs, replays them into
phase-aware effective traces, canonicalizes each Mazurkiewicz trace class by
the lexicographically minimal topological order of the checker's DPOR
dependence DAG, and asserts `class_count <= dpor <= naive`. It also checks that
every schedule in a canonical class replays to the same public verdict kind,
which re-validates the independence relation behind the pruning argument. TSO
and PSO use dedicated meter corpora because internal flush transitions change
the transition alphabet and class-count argument. The cross-model
`model_inclusion` gate checks the separate semantic theorem that per-kind bug
existence is monotone from SC to TSO to PSO, excluding capped or residual-bound
runs where truncation would invalidate the implication.

## Pruning-Certificate Status

The checker does not currently emit or certify pruning-soundness
certificates. ADR 0031 records the Campaign 17 diagnosis and deferral.

The decisive mismatch is in disabled-transition repair. The backward scan can
use a positive independence result before discovering that the later exact
occurrence is disabled at the candidate prefix. A two-order semantic checker
cannot execute that occurrence from the recorded state, while trusting the
existing independence predicate would place the audited heuristic inside the
certifier's trust base. Changing the reducer to make all such claims
co-enabled can change explored schedules and requires a separate campaign.

Nor is replay plus an explorer-authored obligation log a pruning certificate.
It can check recorded leaves and recorded demands, but it cannot discover a
demand the producer omitted. Sleep provenance additionally depends on exact
occurrence survival and semantic commutation. Any future certifier must
independently reconstruct enabled occurrences, clocks, HB, and reversal
obligations; either preserve ordered race histories in its verdict state or
prove an explicit existential-verdict quotient; distinguish executed edges
from terminal, bound-attempt, and cycle-cut dispositions; and compare
intermediate enabledness in every supported local diamond.

Until those conditions are met, the verification trust base remains the
naive/DPOR differential gates, replayable public witnesses, fixed-seed fuzz,
cross-model inclusion, and the optimality meter. No CLI syntax or public API
name is reserved for an unimplemented certificate theorem.

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
