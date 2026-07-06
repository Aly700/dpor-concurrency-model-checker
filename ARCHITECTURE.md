# Architecture — DPOR Model Checker

## Pipeline

```text
.dpor text program -> CLI parser -> model::Program -> scheduler -> explored schedules -> HB/race/deadlock analysis -> minimal replay -> CLI report/replay schedule
```

## Current scaffold

- `model::Program` is a tiny action IR.
- `dpor` is a deterministic CLI consumer that parses `.dpor` text programs,
  runs check or replay through the public `ModelChecker` API, and renders
  stable reports plus replayable schedule blocks.
- `ModelChecker::explore_naive` is the exhaustive oracle.
- `ModelChecker::explore_dpor` is the reduced oracle added beside the naive
  DFS. It uses deterministic backtrack/done sets and relies exclusively on
  `independent()` when pruning equivalent orderings.
- `ModelChecker::replay` re-executes a deterministic schedule and rejects
  disabled or out-of-range steps with a clear error.
- `ModelChecker::minimize_schedule` greedily deletes per-thread tail steps only
  when replay proves the same race, deadlock, or modeled-error identity still
  reproduces; public exploration reports are normalized through it before
  return.
- `VectorClock` is the base happens-before data structure.
- The cross-validation executable `dpor_oracle` checks a deterministic capped
  family of tiny programs against the naive oracle.

## Phase 1 execution model

The checker interprets a program as a small-step state machine:

- `pc[tid]` is the next action for each modeled thread.
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
- `wait_phase[tid]` records whether a thread is not waiting, asleep in a
  condition variable, or woken and waiting to reacquire its mutex.
- `memory[address]` records the last plain write, the plain reads since that
  write, and deterministic plain/atomic access lists used only for the mixed
  plain/atomic race check.

At each DFS state, the naive oracle enumerates exactly the enabled actions in
ascending thread-id order. `Read`, `Write`, `AtomicLoad`, `AtomicStore`,
`AtomicRmw`, `Yield`, `Unlock`, `Signal`, and `Broadcast` are enabled; invalid
`Unlock`, invalid `Wait`, and invalid `Join` steps are reported as modeled
errors. `Lock` is enabled only when its mutex is not currently owned.
`Join(target)` is enabled only when `target` has finished.
`Wait(cv, mutex)` is one IR action with two schedule steps at the same action
index: release-and-sleep, then after wakeup reacquire-and-resume. A state with
unfinished threads and no enabled action is a deadlock; the report tags each
thread as waiting on a mutex, join target, or condition variable. A state where
all threads are finished is normal termination.

## Happens-Before Analysis

Each executed step ticks its thread's vector clock. `Unlock` stores the
releasing thread's clock in the mutex clock. `Lock` joins the acquiring thread's
clock with that mutex clock. `Wait` first performs the same release update as
`Unlock`, then its woken second step performs the same acquire join as `Lock`.
`Signal` and `Broadcast` join the signaler's clock into each woken waiter.
`Join` joins the caller's clock with the target thread's final clock.
`AtomicStore(address)` replaces the address's atomic location clock with the
storing thread's post-tick clock and does not acquire from the old clock.
`AtomicLoad(address)` joins the loading thread with that location clock and
does not mutate it. `AtomicRmw(address)` joins the current location clock into
the executing thread, then replaces the location clock with the joined thread
clock. Store and RMW replacement is deliberate: joining old location clocks
would fabricate happens-before edges and can hide real plain-data races.

Memory accesses compare their current vector clock against prior conflicting
accesses to the same address. Plain/plain races use the existing last-write and
reads-since-last-write metadata. Atomic/atomic accesses never race. Mixed
plain/atomic accesses race when unordered by happens-before and at least one
side is write-like: plain write, atomic store, or atomic RMW. A race report
records the two access endpoints and the executed prefix through the second
access.

The exhaustive oracle explores all enabled interleavings of the modeled
small-step semantics. Therefore, per-execution happens-before race detection
over every explored schedule is complete for this model: if a modeled race or
deadlock is reachable in any enabled interleaving, the naive oracle will visit a
prefix that reports it.

## DPOR Reduction

DPOR maintains a stack of prefix nodes with sorted enabled, backtrack, done, and
sleep thread sets. Each enabled node records both the replay endpoint and the
phase-aware effective action for each enabled thread, so `Wait` release/sleep
and woken mutex reacquire are reduced as different transition semantics while
public schedules remain `(thread, action_index)` pairs.

Dynamic backtracking follows the Flanagan-Godefroid last-point rule for enabled
transitions: it adds the later thread only at the last earlier dependent
transition that is not happens-before ordered before it. Each trace entry stores
the executing thread's post-step vector clock for this check. If the later
effective transition was not enabled at the candidate prefix, DPOR keeps the
classic disabled-transition fallback and adds every enabled thread at a repair
point.

Godefroid sleep sets prune alternatives whose trace class has already been
represented. A child inherits slept threads only while their current effective
next action is still independent of the transition just executed; slept threads
are never executed, and explored choices are added to the node sleep set for
later alternatives. Modeled-error endpoints still clear pruning at that node and
add every enabled sibling, and sleep-blocked prefixes still apply the disabled
fallback before being pruned.

Race and error detection still run through the same interpreter as naive
exploration, and every DPOR report is expected to replay identically.

Atomics do not add blocking or phase changes. `effective_next_action()` passes
them through unchanged, and sleep-set inheritance plus happens-before-aware
backtracking rely only on the updated `independent()` and `may_conflict()`
clauses. Two same-address atomic loads are independent; same-address pairs
involving atomic store or RMW are dependent; and same-address mixed
plain/atomic pairs are dependent.

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
