# ADR 0029: Mesa condition-variable Broadcast

## Status

Accepted.

Campaign 15 completes the existing Mesa condition-variable namespace with a
campaign-strength Broadcast contract: exact fan-out HB to every waiter parked
at the firing point, no waiter-waiter edge, no permit or clock retained by an
empty Broadcast, conservative same-condition dependence, and exact waking-set
occurrence identity at DPOR's historical matching boundaries.

The campaign initially stopped after comparing all 28 baseline suites with all
29 campaign suites. The orchestrator rejected that partial deferral because it
changed the measured unit: most of the observed difference came from newly
required test and CLI work, not a like-for-like checker-core workload. Under
ADR 0020's accounting precedent, the corrected comparison uses the exact
27-suite name intersection and separately reports every named exploration-core
gate. The campaign best-of-three is 19.87 seconds versus 20.11 seconds at
baseline, and every core-gate best is equal or lower. The no-regression gate
therefore passes.

## Context

Loop 4 introduced Mesa condition variables with:

- `Wait(condition, mutex)` as a two-phase action under one stable
  `(thread, action_index)`;
- an atomic first phase that releases the mutex and parks the caller;
- a second phase in which a woken waiter contends for the same mutex before
  the source pc advances;
- `Signal(condition)`, which wakes one currently parked waiter;
- no queued permit, so an empty signal is forgotten and a later wait can
  deadlock; and
- a wake edge through which the waiter joins the signaler's clock at the wake
  point.

Baseline `83e8cf9` already contained a `Broadcast` action kind, strict
`broadcast CONDITION` parser and renderer support, interpreter fan-out over
the current waiter set, the same ordered-point treatment as `Signal` under TSO
and PSO, and a basic condition-variable test. Campaign 15 did not invent that
surface. It made the semantics discriminating, widened every oracle family,
and closed the DPOR occurrence-identity gap exposed by a multi-wake action.

That gap matters when a backward branch returns to one static Broadcast
endpoint. The endpoint may fire once with an empty condition and later with
one or more parked waiters. Those occurrences have different effects even
though `(thread, action_index, action)` is identical. Barrier releases already
carry their generation in occurrence identity. Broadcast requires the exact
set it wakes.

## Decision and design

### Surface and Mesa behavior

The canonical source spelling is:

```text
broadcast CONDITION
```

Broadcast never blocks. Let `W(cv)` be the canonical sorted set of threads
parked, rather than already reacquiring, on condition `cv` at the firing
point.

For a nonempty set, Broadcast atomically:

1. snapshots `W(cv)`;
2. removes those threads from the parked set;
3. moves every member to the mutex-reacquisition phase of its existing Wait;
   and
4. advances and ticks the broadcaster as an ordinary source action.

It does not grant mutex ownership. Woken waiters subsequently contend through
the existing second Wait phase, so different reacquisition orders remain
ordinary mutex-dependent executions.

For `W(cv) = {}`, Broadcast performs only its own source-action progress. It
records no permit, release clock, or condition history. A Wait issued later
still parks, and a terminal state containing that waiter remains a
lost-wakeup deadlock.

### Happens-before fan-out

Let `C_b+` be the broadcaster's post-tick clock at the firing point. For every
`w` in the snapshot `W(cv)`, independently apply the Signal wake equation:

```text
C_w' = C_w join C_b+
```

Every receiver joins the same broadcaster clock. Waking `w0` does not mutate
the source clock used to wake `w1`, and no receiver clock is joined into
another receiver. Broadcast is a fan-out of one edge, not a chain among the
waiters.

Any order between two woken waiters arises later through actual mutex
reacquisition and release edges. A negative leak probe therefore places
conflicting plain accesses after their critical sections and requires the race
to remain.

When `W(cv)` is empty, the loop has no iterations and no condition clock
retains `C_b+`. A later Signal may wake a later waiter, but cannot carry the
earlier empty Broadcast's clock.

### TSO and PSO

Broadcast is an ordered synchronization source action under both buffered
models, exactly like Signal. It remains disabled while the broadcaster owns a
pending store-buffer entry. Only an explicit TSO or PSO flush transition can
drain that entry; Broadcast performs no hidden flush.

Focused probes reject Broadcast before the explicit drain and accept it after
the drain under both buffered models.

### Dependence

The public relation remains deliberately conservative:

- same-condition `Wait`, `Signal`, and `Broadcast` are pairwise dependent;
- operations on distinct conditions are independent, subject to existing
  cross-cutting thread, mutex, spawn/join, and buffered-transition rules; and
- the optional empty-Broadcast/empty-Broadcast refinement is not implemented.

Even two currently empty Broadcasts remain dependent. The campaign has no
occurrence-exact proof that such a refinement is safe at every node,
backtrack, repair, and sleep boundary, and it is unnecessary for the meter.

### Exact waking-set occurrence identity

Each enabled or executed `Broadcast(cv)` carries the exact canonical set of
threads parked on `cv` at its firing point:

```text
non-Broadcast transition: no waking-set component
empty Broadcast:          engaged exact set {}
nonempty Broadcast:       engaged exact sorted set {t0, ..., tn}
```

Engaged-empty is distinct from absent. The set is a sorted vector of
`ThreadId`, not a cardinality or hash. Internally it is stored behind an
immutable shared value to keep the non-Broadcast path small. Equality is
structural; pointer identity never participates. Public effective schedule
steps carry an owned exact vector for replay diagnostics and tests.

The complete occurrence bundle is used in:

1. enabled and executed transition records;
2. matching an enabled transition against its DPOR node;
3. backtrack insertion and persistent-set closure;
4. disabled-transition repair;
5. checker-local state-sensitive independence; and
6. sleep-set inheritance into the child.

The sleep-inheritance audit alone would not require this component under the
accepted conservative relation. The only transitions that mutate
`condition_waiters[cv]` are the first phase of `Wait(cv, m)`, `Signal(cv)`,
and `Broadcast(cv)`, and every same-condition pair is dependent. A slept
Broadcast can therefore cross only parent-independent transitions that leave
its parked set unchanged. This argument deliberately relies on rejecting the
optional empty-Broadcast refinement.

Historical backtrack matching is different. A backward branch can return a
broadcaster to the same public endpoint after the parked set changes. Without
the set component, a later occurrence may falsely match an earlier Broadcast
solely because endpoint and action agree. That match can bypass
disabled-transition repair and take the direct-backtrack or HB-skip path.
Initial persistent closure does not subsume the check: it runs only for an
initially empty backtrack set, skips slept candidates, and is not rerun after
dynamic insertion. Behavioral fingerprints distinguish waiter sets for lasso
detection, but are not consulted by this transition-matching predicate.

A two-direction mutation fixture makes this safeguard falsifiable. The
broadcaster executes a finite two-iteration
`AtomicStore; Broadcast; AtomicRmw; BranchNonzero` loop, the waiter executes
`Lock; Wait; Write; Unlock`, and a third thread reads the written location.
`LOW` assigns thread IDs broadcaster/waiter/reader; `HIGH` assigns
reader/waiter/broadcaster.

| Layout | Naive | Exact-stamp DPOR | Stamp-suppressed DPOR |
|---|---:|---:|---:|
| `LOW` | 3,954 | 16 | 9 |
| `HIGH` | 3,954 | 25 | 19 |

Both mutations still expose the fixture's existential race and deadlock
kinds. The test therefore witnesses loss in occurrence-sensitive
backtrack/class accounting in both thread-ID directions, not a demonstrated
existential-verdict false negative. No occurrence-exact proof establishes
that the discarded representatives are equivalent. Retaining the exact stamp
is the soundness-preserving choice: the checker may overexplore, but must not
silently prune an unproved class.

### Reacquisition orders

Waking multiple waiters changes only their Wait phase. Each effective second
phase remains the existing mutex acquisition at the same source endpoint, and
ordinary same-mutex dependence controls the contention.

The discriminator has two parked waiters whose post-wake critical sections
write a protected verdict value in opposite orders. It pins 22 naive
schedules and 14 DPOR schedules and explicitly replays both reacquisition
orders. Its outcome set includes the intended distinct verdict classes, so it
rejects a reducer that retains only one mutex winner.

### Behavioral fingerprints and ADR 0023

Broadcast needs no new behavioral-fingerprint field.

For a nonempty Broadcast, the already fingerprinted condition waiter set and
per-thread Wait phase change, and the broadcaster's normalized pc advances.
For an empty Broadcast, the broadcaster's normalized pc still advances.
Reacquisition later changes the already fingerprinted mutex owner, Wait phase,
and waiter pc. Wake clocks and the occurrence stamp are analysis metadata, not
program-observable state.

ADR 0023's acyclic-elision proof is extended by a classifier note:
Broadcast is a fired source action that always advances normalized pc. It is
not a same-pc phase transition like a non-last barrier arrival, and it cannot
restore an earlier behavioral state without an already-detected backward
branch.

## Verification

### Baseline

Before campaign changes, the required smoke command passed all 28 tests at
`83e8cf9`:

```text
cmake --build build -j8 && ctest --test-dir build
100% tests passed, 0 tests failed out of 28
```

The unchanged optimality binary printed the predeclared meter lines:

```text
SC  1.067
TSO 1.152
PSO 1.154
```

### Focused semantics and discriminators

The twenty-ninth suite covers:

- exact `{}`, `{T1}`, and `{T1, T2}` public waking-set identities;
- fan-out to every currently parked waiter;
- no stored permit after an empty Broadcast;
- repeated static endpoints and the two-direction stamp-removal mutation;
- 22-naive/14-DPOR mutex-reacquisition order discrimination;
- direct two-Signal replay in which one waiter can re-wait and consume the
  second Signal; and
- ordered-point rejection before, and success after, explicit TSO and PSO
  drains.

The forced-parking differential makes the first Signal occur before either
consumer is spawned. Once both consumers are parked, one Broadcast wakes both
and is clean at 86 naive / 30 DPOR schedules. The two-Signal variant has
already lost its first wake and leaves one consumer parked after the last
Signal; it exposes a deadlock at 43 naive / 15 DPOR schedules. Both explorers,
library replay, and CLI check pin that distinction.

The classic gallery records the same comparison:

```text
mesa_broadcast_consumers.dpor
mesa_broadcast_consumers_broken_single_signal.dpor
```

Their DPOR stdout goldens contain 30 clean and 15 deadlocking schedules,
respectively.

### Happens-before mutation evidence

The positive HB gate mirrors thread IDs for both broadcaster-to-waiter
directions. Removing only the `wake_waiter` join makes the first positive
Broadcast probe report the intended race; bypassing that assertion exposes
the mirrored flip. The join and both assertions are restored.

The unmutated negative probes require:

- two waiters woken by one Broadcast to remain unordered outside their later
  critical sections;
- an empty Broadcast to contribute no clock to a later Signal-woken waiter;
  and
- a Wait issued after an empty Broadcast to remain a replayable deadlock.

### Widened deterministic gates

The Broadcast-widened Release gates report:

| Gate | Programs | Alphabet | Naive schedules | DPOR schedules | Skips |
|---|---:|---:|---:|---:|---:|
| SC two-thread | 22,736 | 27 | 59,119 | 34,419 | 0 |
| SC three-thread | 65,547 | 25 | 789,230 | 331,445 | 0 |
| TSO | 11,740 | 22 | 59,688 | 19,517 | 0 |
| PSO | 6,621 | 22 | 31,441 | 10,622 | 0 |

The TSO and PSO alphabets now include `Wait`, `Signal`, and `Broadcast`.
The SC alphabets already contained all three and have exact presence guards so
silently dropping Broadcast fails the gate.

Fixed-seed fuzz generates 3,000 programs, fully compares 2,978, and reports 22
capped programs. It generates 762 Broadcast actions and compares 746:
737/721 in the mostly-well-formed lane and 25/25 in the adversarial lane.
The gate requires at least 300 generated Broadcasts in aggregate and at least
75 percent generated-to-compared coverage in every non-value lane.

Cross-model inclusion compares 1,741 programs with zero skips and runs 17,410
checks. Its dedicated Broadcast corpus reports 4 attempted, 4 compared, and
0 skipped.

### Corrected like-for-like wall-time gate

The first timing experiment compared a 28-suite baseline with a 29-suite
campaign and measured 20.71 seconds versus 21.04 seconds. That 0.33-second
difference included the new focused suite and additional CLI coverage. It was
not a like-for-like measurement of checker-core cost. The orchestrator ruling,
following ADR 0020's accounting precedent, retains that result as measurement
history but does not use it to decide this gate.

A pristine `83e8cf9` Release build and the campaign Release build then ran the
exact 27-suite name intersection, excluding only `cli_tests` and
`condvar_broadcast_tests`. Runs were serial and used the accepted fixed order:

```text
baseline, campaign, campaign, baseline, baseline, campaign
```

Every sample passed 27/27.

| Common-suite Release `ctest` | Sample 1 | Sample 2 | Sample 3 | Best |
|---|---:|---:|---:|---:|
| Baseline `83e8cf9` | 25.15s | 20.11s | 20.18s | 20.11s |
| Campaign 15 | 20.30s | 19.99s | 19.87s | 19.87s |

The campaign best is 0.24 seconds (1.19 percent) faster. This satisfies the
no-regression gate; the claim is no regression, not a material speedup.

Per-suite times from those same full-suite samples show no regression in any
named exploration-core gate:

| Suite | Baseline samples | Campaign samples | Baseline best | Campaign best | Best delta |
|---|---|---|---:|---:|---:|
| `dpor_oracle` | 0.46 / 0.31 / 0.31s | 0.31 / 0.32 / 0.30s | 0.31s | 0.30s | -0.01s |
| `dpor_optimality` | 1.72 / 1.54 / 1.54s | 1.53 / 1.62 / 1.55s | 1.54s | 1.53s | -0.01s |
| `dpor_oracle_3threads` | 3.30 / 3.03 / 3.14s | 3.01 / 3.07 / 3.07s | 3.03s | 3.01s | -0.02s |
| `tso_oracle` | 0.46 / 0.29 / 0.31s | 0.26 / 0.26 / 0.26s | 0.29s | 0.26s | -0.03s |
| `pso_oracle` | 0.37 / 0.18 / 0.19s | 0.14 / 0.14 / 0.15s | 0.18s | 0.14s | -0.04s |
| `dpor_fuzz_differential` | 2.59 / 2.46 / 2.42s | 2.52 / 2.40 / 2.38s | 2.42s | 2.38s | -0.04s |

This is suite-name like-for-like, not corpus-identical.
`classic_gallery_tests` remains in the primary intersection because it exists
in both trees, although the campaign adds the Broadcast gallery pair. The
oracle and fuzz gates also add Broadcast coverage. No added case was removed
to improve timing. The separate core-gate table makes the relevant result
explicit: all six best times are lower, and their small differences fall
within ADR 0023's host-noise regime.

### Final acceptance matrix

The final source state passed all of these fresh checks:

- complete Release (29/29 in 21.16 seconds) and restore-assert Debug (29/29
  in 235.00 seconds) suites;
- byte-identical SC 1.067 / TSO 1.152 / PSO 1.154 meter lines;
- focused semantics, both HB mutation directions, negative leak probes, and
  both class-pinned discriminators;
- four widened zero-skip oracle gates, fixed-seed fuzz coverage, and
  zero-skip cross-model inclusion; and
- strict CLI check/replay probes plus gallery goldens.

A direct diff of the complete `dpor_optimality` stdout against the pristine
`83e8cf9` binary was empty. Fresh verifier-written CLI programs separately
proved clean two-receiver publication through Broadcast and replayed an empty
Broadcast followed by a parked waiter as the expected deadlock.

## Decision

Accept Campaign 15.

Broadcast uses the existing Mesa two-phase Wait machinery and wakes the exact
set parked at its firing point. Every receiver independently joins the
broadcaster's clock; no receiver clock flows to another receiver. Empty
Broadcast is forgotten. TSO and PSO require explicit buffer drain.
Same-condition Wait, Signal, and Broadcast remain conservatively dependent,
and the optional empty-Broadcast refinement is rejected.

Retain the exact sorted waking-set occurrence stamp across enabled/executed
records, node matching, backtracking, repair, state-sensitive independence,
and sleep inheritance. The stamp is structural and collision-free;
engaged-empty differs from absent. No new behavioral fingerprint field is
added because every fired Broadcast advances normalized pc and nonempty
fan-out also changes already fingerprinted waiter phases.

## Consequences

- One Broadcast wakes every waiter parked on its condition at that firing
  point, without pre-acquiring any waiter's mutex.
- Every woken waiter receives the broadcaster's HB edge independently.
- Broadcast itself creates no waiter-waiter ordering.
- Empty Broadcast stores neither a permit nor a clock; later Wait still parks.
- Exact fan-out participates in DPOR occurrence identity but not in numeric
  schedules or behavioral fingerprints.
- Conservative same-condition dependence makes the sleep audit simple and
  protects disabled-transition repair; exact identity separately protects
  cyclic historical matching.
- Multi-wake mutex contention remains ordinary two-phase Wait behavior and
  retains verdict-relevant reacquisition classes.
- Release, Debug restore assertions, optimality meters, deterministic
  oracles, fuzz, inclusion, CLI, and gallery coverage remain shipment gates.

## Invariants protected

- **HB soundness:** Broadcast fans out one broadcaster clock without
  constructing receiver order or retaining an empty wake.
- **DPOR soundness:** one static Broadcast endpoint cannot stand for
  occurrences with different exact parked sets.
- **Mesa semantics:** neither Signal nor Broadcast queues a future permit.
- **Replay:** waking sets, reacquisition classes, and lost-wakeup blockers
  retain original thread coordinates.
- **Weak memory:** Broadcast remains an explicit-drain ordered point under TSO
  and PSO.
- **Fingerprinting:** existing observable waiter phase and pc fields suffice;
  analysis-only clock and occurrence metadata stay excluded.
- **Acceptance accounting:** new test-coverage cost is not mislabeled as
  checker-core regression; the core is measured on a fixed common-suite unit.
