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
  `independent()` predicate, and checker-local transition refinements for
  enabled valid `Join` operations, failed same-mutex `TryLock` pairs under a
  third-party owner, and generation-stamped barrier arrivals.
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
`BranchNonzero`, `Assert`, `Fence`, `Yield`, `TryLock`, `Unlock`, `RUnlock`,
`WUnlock`, `SemPost`, `Spawn`, `Signal`, and
`Broadcast` are enabled for started unfinished threads; invalid `Unlock`,
invalid rwlock unlock/reentrancy, invalid `Wait`, invalid `Spawn`, and invalid
`Join` steps are reported as modeled errors. `Lock` is enabled only when its
mutex is not currently owned; `TryLock` remains enabled regardless of the
owner and reports failure through its register result. `RLock` is enabled in
the absence of another thread's writer; `WLock` is enabled only with no writer
and no readers, except that a current writer's recursive acquisition remains
executable as an error.
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
index: release-and-sleep, then after wakeup reacquire-and-resume. A state with
started unfinished threads and no enabled action is a deadlock; the report
tags each started blocked thread as waiting on a mutex, join target, condition
variable, rwlock writer, rwlock reader set, semaphore, or incomplete barrier
generation. A state where all started
threads are finished is normal
termination, even if some static thread bodies were never spawned. If an
enabled choice names a thread that has already executed its per-thread step
bound, that execution terminates with a bound outcome and increments
`bound_exceeded_executions`.

Under TSO and PSO, atomics, CAS, mutex/condition-variable operations (including
`TryLock`), all four
rwlock operations, both semaphore operations, barrier waits, spawn, join, and
`Fence` are ordered points: they are
disabled until all of the
executing thread's buffers are empty. A nonempty buffer always enables a flush for that thread,
including after the source pc is done, so buffered completion is not deadlock.

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

## Happens-Before Analysis

Each executed step ticks its thread's vector clock. `Unlock` stores the
releasing thread's clock in the mutex clock. `Lock` joins the acquiring thread's
clock with that mutex clock. A successful `TryLock` performs exactly that same
join after becoming owner. A failed `TryLock` performs no join at all: neither
the stored release clock nor the current owner's live clock is acquired.
`Wait` first performs the same release update as
`Unlock`, then its woken second step performs the same acquire join as `Lock`.
`Signal` and `Broadcast` join the signaler's clock into each woken waiter.
`RLock` joins only the named rwlock's last writer-release clock. `RUnlock`
joins its post-tick clock into the rwlock's reader-release accumulator.
`WLock` joins both the last writer release and every accumulated reader
release, then clears the reader accumulator for the next epoch. `WUnlock`
replaces the writer-release clock with its post-tick clock. In particular,
readers never acquire one another's release clocks.
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
loops.

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

Programs without a normalized self/backward `BranchNonzero` do not allocate or
construct cycle history. Every ordinary source step advances a normalized pc;
`TryLock` does so on both outcomes, with its result already represented by the
existing register array and successful ownership by `mutex_owner`;
the exceptional first `Wait` phase changes the fingerprinted wait/ownership
state and needs a later pc-advancing signal before it can resume; a non-last
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

## DPOR Reduction

DPOR maintains a stack of prefix nodes with sorted enabled, backtrack, done, and
sleep schedule-step sets. Each enabled node records both the replay endpoint and the
phase-aware effective action for each enabled thread, so `Wait` release/sleep
and woken mutex reacquire are reduced as different transition semantics while
source and TSO schedule steps remain `(thread, action_index)` pairs. Barrier
occurrences additionally carry the current generation ordinal internally,
because a cyclic program can execute the same `(thread, action_index)` in more
than one generation.

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

Two cross-thread, co-enabled successful `RLock` operations on one name are
independent. Either order leaves the same reader set, thread clocks,
synchronization clocks, shared/race state, and enabled set; a waiting writer is
disabled after either first acquisition and after both. All other same-rwlock
pairs stay dependent in the public relation. A checker-local refinement
commutes every cross-thread reader-mode pair only when the complete program has
no writer-mode action on that name. The static absence of a writer removes the
middle witness in which the last `RUnlock` enables a third thread's `WLock`
between two otherwise commuting reader transitions.

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
oracle sweep enumerates small programs by length pair over a 25-action alphabet
and compares naive vs. DPOR race/deadlock/error/assertion existence, the
bound-hit boolean, total-cycle existence, all three fairness-class existence
booleans, schedule dominance, and report replay identity. The
3-thread oracle sweep uses an evenly strided deterministic sample of the larger
23-action, 6-slot space, plus hand-picked disabled-transition cases, to
exercise spawn, join, condition-variable, rwlock, semaphore, and barrier
enabledness. Fixed spin and mutex-blink lassos keep its weak, strong, and fair
class comparisons non-vacuous. A
separate three-reader fixture pins 1680 naive leaves and one DPOR
representative. The seeded differential fuzz gate generates 3000 fixed-seed
programs across 2-5 threads, 1-6 actions per thread, plain and atomic memory,
mutexes, rwlocks, semaphores, barriers, TryLock, condition variables, joins,
yields, spawn-shaped programs, and modeled-error cases, plus a value-mode lane
with registers, branches, CAS, fetch-add, assertions, and deliberate bound
hits; capped explorations are counted but excluded from verdict equality
because truncation can legitimately hide a later endpoint. Fuzz compares all
three lasso classes and runs a fixed uncapped mutex-blink discriminator even
when the seeded corpus contains no strong-class witness.

The strong-fairness-widened acceptance run checked 22,419 two-thread programs
(61,091 naive versus 34,111 DPOR schedules), 65,546 three-thread programs
(896,259 naive versus 347,251 DPOR schedules), 10,776 TSO programs (136,101
versus 28,030), and 5,657 PSO programs (85,820 versus 15,107). Every oracle
observed fair, strongly-unfair, and weakly-unfair cycle existence under both
naive and DPOR exploration. Both buffered oracles
enforce zero capped skips. Their action alphabets were respectively 25, 23, 13,
and 13. The fixed fuzz run generated 952 `BarrierWait` and 1,578 `TryLock`
actions, with 1,556 TryLocks in
fully compared programs; 2,983 of its 3,000 programs were compared and 17
capped programs were reported. Cross-model inclusion compared all 1,723
programs with zero global skips, including both dedicated four-program
BarrierWait and TryLock corpora at 4/4/0. The unchanged
optimality corpora retain byte-identical meter ratios SC 1.067, TSO 1.152, and
PSO 1.154.

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

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
