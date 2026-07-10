# Architecture — DPOR Model Checker

## Pipeline

```text
.dpor text program -> CLI parser -> model::Program -> scheduler -> explored schedules -> HB/race/deadlock analysis -> minimal replay -> CLI report/replay schedule
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
  `independent()` predicate, and a checker-local transition refinement for
  enabled valid `Join` operations.
- `ModelChecker::replay` re-executes a deterministic schedule under the same
  step bound and rejects disabled, out-of-range, or post-terminal steps with a
  clear error.
- `ModelChecker::minimize_schedule` greedily deletes per-thread tail steps only
  when replay proves the same race, deadlock, modeled-error, or assertion
  identity still reproduces; public exploration reports are normalized through
  it before return.
- `VectorClock` is the base happens-before data structure.
- The cross-validation executable `dpor_oracle` checks a deterministic capped
  family of tiny programs against the naive oracle.

## Phase 1 execution model

The checker interprets a program as a small-step state machine:

- `pc[tid]` is the next executable action for each modeled thread, normalized
  past label pseudo-actions.
- `registers[tid][0..7]` are thread-local int64 registers initialized to zero.
- `thread_steps[tid]` counts scheduled steps for the per-thread execution
  bound.
- `mutex_owner[mutex]` records the owning thread for held mutexes.
- `mutex_clock[mutex]` records the vector clock stored by the last successful
  unlock of that mutex.
- `atomic_location_clock[address]` records the per-address release sequence for
  modeled atomic operations. The model supports acquire loads, release stores,
  acquire-release RMWs, and SC-per-location; it does not model relaxed atomics.
- `condition_waiters[cv]` records sleeping waiters in deterministic thread-id
  order. Signal wakes the lowest-numbered waiter; broadcast wakes all current
  waiters. No permits are queued when the set is empty.
- `thread_clock[tid]` records each thread's happens-before frontier.
- `started[tid]` records whether a static thread body is alive in the current
  execution. Threads targeted by valid `Spawn` actions start disabled and
  become schedulable only after a successful spawn.
- `wait_phase[tid]` records whether a thread is not waiting, asleep in a
  condition variable, or woken and waiting to reacquire its mutex.
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
`BranchNonzero`, `Assert`, `Fence`, `Yield`, `Unlock`, `Spawn`, `Signal`, and
`Broadcast` are enabled for started unfinished threads; invalid `Unlock`,
invalid `Wait`, invalid `Spawn`, and invalid `Join` steps are reported as
modeled errors. `Lock` is enabled only when its mutex is not currently owned.
`Join(target)` is enabled only when `target` has started and finished.
`Wait(cv, mutex)` is one IR action with two schedule steps at the same action
index: release-and-sleep, then after wakeup reacquire-and-resume. A state with
started unfinished threads and no enabled action is a deadlock; the report
tags each started blocked thread as waiting on a mutex, join target, or
condition variable. A state where all started threads are finished is normal
termination, even if some static thread bodies were never spawned. If an
enabled choice names a thread that has already executed its per-thread step
bound, that execution terminates with a bound outcome and increments
`bound_exceeded_executions`.

Under TSO and PSO, atomics, CAS, mutex/condition-variable operations, spawn,
join, and `Fence` are ordered points: they are disabled until all of the
executing thread's buffers are empty. A nonempty buffer always enables a flush for that thread,
including after the source pc is done, so buffered completion is not deadlock.

## Happens-Before Analysis

Each executed step ticks its thread's vector clock. `Unlock` stores the
releasing thread's clock in the mutex clock. `Lock` joins the acquiring thread's
clock with that mutex clock. `Wait` first performs the same release update as
`Unlock`, then its woken second step performs the same acquire join as `Lock`.
`Signal` and `Broadcast` join the signaler's clock into each woken waiter.
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
stores on success and writes `1` or `0` to its result register.

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

## DPOR Reduction

DPOR maintains a stack of prefix nodes with sorted enabled, backtrack, done, and
sleep schedule-step sets. Each enabled node records both the replay endpoint and the
phase-aware effective action for each enabled thread, so `Wait` release/sleep
and woken mutex reacquire are reduced as different transition semantics while
source and TSO schedule steps remain `(thread, action_index)` pairs.

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
prefix, DPOR first tries an enabler-chain repair. It adds only enabled threads
that can advance the disabled transition's concrete chain: a remaining
`Spawn(t)` for a not-started thread, the target's remaining execution for
`Join(t)`, a same-thread prerequisite, a mutex owner inside such a chain, or a
`Signal`/`Broadcast` chain for a sleeping waiter. If the chain is not proven,
or if the top-level disabled transition is a blocked `Lock` or woken wait
reacquire, DPOR falls back to ADR 0010's all-enabled repair.

Godefroid sleep sets prune alternatives whose trace class has already been
represented. A child inherits slept threads only while their current effective
next action is still independent of the transition just executed; slept threads
are never executed, and explored choices are added to the node sleep set for
later alternatives. Modeled-error endpoints still clear pruning at that node and
add every enabled sibling, and sleep-blocked prefixes still apply the disabled
fallback before being pruned.

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

The DPOR implementation is checked against deterministic gates. The
2-thread oracle sweep enumerates small programs by length pair over a 17-action
alphabet and compares naive vs. DPOR race/deadlock/error/assertion existence,
the bound-hit boolean, schedule dominance, and report replay identity. The
3-thread oracle sweep uses an evenly strided deterministic sample of the larger
15-action, 6-slot space, plus hand-picked disabled-transition cases, to
exercise spawn, join, and condition-variable enabledness. The seeded
differential fuzz gate generates 3000 fixed-seed programs across 2-5 threads,
1-6 actions per thread, plain and atomic memory, mutexes, condition variables,
joins, yields, spawn-shaped programs, and modeled-error cases, plus a
value-mode lane with registers, branches, CAS, fetch-add, assertions, and
deliberate bound hits; capped explorations are counted but excluded from
verdict equality because truncation can legitimately hide a later endpoint.

The optimality meter is the fourth gate and remains SC-only. It collects all naive maximal
schedules for small non-error/non-assertion programs, replays them into
phase-aware effective traces, canonicalizes each Mazurkiewicz trace class by
the lexicographically minimal topological order of the checker's DPOR
dependence DAG, and asserts `class_count <= dpor <= naive`. It also checks that
every schedule in a canonical class replays to the same public verdict kind,
which re-validates the independence relation behind the pruning argument.
TSO and PSO have separate differential oracles because internal flush
transitions change the transition alphabet and class-count argument. The
cross-model `model_inclusion` gate checks the separate semantic theorem that
per-kind bug existence is monotone from SC to TSO to PSO, excluding capped or
residual-bound runs where truncation would invalidate the implication.

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
