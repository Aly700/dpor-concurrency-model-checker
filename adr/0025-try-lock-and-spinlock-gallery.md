# ADR 0025: Try-lock, failed-attempt independence, and the spinlock gallery

## Status

Accepted.

## Context

The model had blocking mutex acquisition but no instruction from which a
program could construct a test-and-set spinlock. `TryLock` adds an observable
success/failure result and never blocks, which makes both its happens-before
semantics and its interaction with lasso detection materially different from
`Lock`.

The acquire edge must be path-sensitive. Success is a real mutex acquisition
and must receive the prior release frontier. Failure is only an observation
that the mutex is held: joining either the stored release clock or the current
holder's live clock would fabricate happens-before and could hide a real race.
Likewise, a retry loop is non-termination under a chosen schedule, not a
deadlock merely because the mutex remains held.

At the action level, operations on one mutex remain dependent. There is one
narrow state-dependent diamond: two threads trying the same mutex while a
third thread owns it must both fail in either order. As with ADR 0024's barrier
refinement, accepting that reduction requires an explicit audit of persistent
closure, parent-snapshot sleep inheritance, disabled-transition repair, and
occurrence identity rather than relying on the local state equality alone.

## Decision

### Action, syntax, namespace, and values

The IR adds `ActionKind::TryLock`. Its strict text spelling is:

```text
try_lock MUTEX -> rN
```

The parser requires exactly those four tokens, the literal lowercase keyword,
the `->` delimiter, and an existing register in the `r0` through `r7` range.
Rendering produces that same canonical spelling, so parse/render/check/replay
round trips remain deterministic.

`TryLock` uses the existing mutex namespace and state. Mutex names remain
exclusive with reader-writer-lock, semaphore, and barrier names under the same
rules as `Lock`, `Unlock`, and the mutex operand of `Wait`; the established
ability for a condition-variable name to reuse a mutex spelling is unchanged.
`Lock` and `TryLock` on one name are intentionally not a collision. Existing
`Unlock` ownership validation is unchanged.

Every attempt ticks once and advances the normalized pc once. If the mutex is
free, the thread becomes owner and the destination register receives exactly
`1`. If any thread owns it, including the caller, ownership is unchanged and
the register receives exactly `0`. Failure never disables the transition and
never creates a blocker. The result flows through the existing register,
operand, assertion, and `BranchNonzero` machinery without a special value
channel.

No behavioral-fingerprint field is added. The register array already records
both outcomes, and `mutex_owner` already records successful ownership. Failure
still advances the pc, while a backward branch restores control to an earlier
attempt in the ordinary way; these existing fields distinguish every
program-observable state needed by lasso detection.

### Happens-before and buffered ordering

Let `C_t` be the attempting thread's clock after the common step tick and
`M_m` the clock stored by the last successful release of mutex `m`.

```text
if m is free:
    owner[m] := t
    register[t][N] := 1
    C_t := C_t join M_m
else:
    register[t][N] := 0
    owner and every mutex clock remain unchanged
    C_t receives no join
```

The success path is therefore exactly `Lock`'s acquire edge. Fixed
verdict-flip schedules cover both thread-id directions after a real release.
Two negative probes pin failure: one keeps a stale prior release frontier while
a different thread owns the mutex, and one places a write in the live holder's
critical section. Both reads after the failed attempt must remain racy, proving
that neither stored nor live-holder clocks leak into the failing thread.

Under TSO and PSO, `TryLock` is a full ordered point like `Lock`. It remains
disabled until all buffers belonging to the arriving thread are explicitly
drained, and it performs no hidden flush on either outcome. Dedicated replay
probes reject an attempt with a pending TSO buffer or any pending PSO address
FIFO, then accept the same source step after explicit drains.

### Deadlock, lasso detection, and fairness

Because ownership never disables `TryLock`, it never appears in a deadlock
blocker set. A program that branches backward on the `0` result can repeat a
behavioral state and is handled by the existing lasso machinery. Under ADR
0018's weak-fairness rule, a cycle containing only the spinner is an
`unfair-schedule witness` when a nonparticipant holder has an enabled `Unlock`
at every replayed cycle state. This makes no universal starvation-freedom
claim; it classifies that exact witness.

ADR 0023's fingerprint-elision classifier needs no new exceptional action.
`TryLock` advances its pc on success and failure, so an acyclic program extends
the ordinary-source well-foundedness proof directly. Any retry necessarily has
a normalized self/backward `BranchNonzero`, which enables cycle history before
the attempt can recur. The profiled classifier gate pins both an elided acyclic
program and a failed-attempt backward loop with balanced history restoration.

### Baseline DPOR dependence

The public `independent(Action, Action)` relation keeps all same-mutex pairs
involving `TryLock`, `Lock`, `Unlock`, `Wait`, or another `TryLock`
conservatively dependent. Different mutex names retain the established
disjoint-resource independence rule. The public predicate has no node owner
snapshot and therefore cannot safely expose the refinement below.

### Third-holder state diamond

At one DPOR node, let different threads `i` and `j` have exact co-enabled
`TryLock(m)` source transitions, and let the node's snapshotted owner be `h`
with `h != i` and `h != j`. The checker-local relation classifies only this pair
independent.

Both attempts fail. In order `i; j` and order `j; i`, each thread ticks once,
advances once, and stores `0` in its own register array. Neither joins a clock
or changes the owner, mutex release frontier, memory values, buffers, race
metadata, or any other resource. The other attempt remains enabled after the
first. After both, the complete execution state and enabled set are identical:

```text
step_i(step_j(S)) == step_j(step_i(S))
```

The refinement rejects an empty mutex name, same-thread pairs, a missing
owner, an owner equal to either trier, any different action or mutex, any
non-source generation stamp, and any endpoint/action that is not the exact
enabled occurrence stored at that node. Thus free-mutex winner branches and
self-owned failures remain dependent.

The DPOR machinery audit is part of the decision:

1. **Persistent closure.** Every mixed same-mutex operation remains dependent
   with both attempts. An intervening `Unlock`, acquisition, or wait/reacquire
   therefore closes the persistent set in the usual way; only the adjacent
   pair of guaranteed failures is commuted.
2. **Sleep inheritance.** Independence is evaluated against the parent node's
   snapshotted owner, not a reconstructed child guess. A slept transition is
   inherited only when its identical endpoint/action remains enabled. Either
   failure leaves the third-party owner unchanged, so the surviving sibling is
   the same occurrence.
3. **Disabled-transition repair.** Mutex ownership never disables `TryLock`.
   TSO/PSO may delay it only through the existing ordered-point buffer rule,
   whose source/flush repair remains conservative. No TryLock-specific enabler
   or all-siblings exception is introduced.
4. **Occurrence identity.** Unlike a parked cyclic barrier arrival, each
   attempt advances its source pc. The existing numeric endpoint and exact
   enabled action identify the transition at a node; a backward revisit either
   forms a detected lasso or reaches a later node snapshot. No generation or
   result stamp is needed.

The exact discriminator is:

```text
T0: try_lock m -> r0
T1: try_lock m -> r0
T2: lock m
```

T2 finishes while retaining `m`. Naive exploration has four terminal orders:
T0 wins before T2, T1 wins before T2, and the two orders of T0/T1 failure after
T2. The first two classes have distinct successful owners/results and remain
dependent. Only the T2-first failures commute, so DPOR explores exactly three
representatives. Guard fixtures retain two representatives on a free mutex,
three when either trier is owner, and all four for a mixed same-mutex action.
A spawn-gated fail/`Unlock`/success assertion and asymmetric winner assertions
pin the middle and outcome classes, not merely the aggregate count.

### Gallery and verification gates

The classic gallery contains a test-and-set spinlock protecting a shared
counter and a paired broken program whose counter write is outside the acquired
section. The correct program has no safety bug; pure-spin schedules are reported
through the lasso/fairness convention above. The broken program produces a
replayable race under naive exploration, DPOR, CLI check, and CLI replay, with
the stable report payload compared byte-for-byte.

The acceptance corpora are widened with `TryLock`:

- two-thread oracle: 22,418 programs over a 25-action alphabet, 61,087 naive
  versus 34,108 DPOR schedules;
- three-thread oracle: 65,544 programs over a 23-action alphabet, 896,252 naive
  versus 347,246 DPOR schedules, including 38,178 strict reductions;
- TSO oracle: 10,775 programs, zero capped skips, over a 13-action alphabet,
  136,097 naive versus 28,027 DPOR schedules;
- PSO oracle: 5,656 programs, zero capped skips, a 13-action alphabet, 85,816
  naive versus 15,104 DPOR schedules;
- deterministic 3,000-program differential fuzz: 1,578 generated `TryLock`
  actions, 1,556 of them in fully compared programs, 2,983 compared programs,
  and 17 explicitly reported capped programs; both generation lanes
  independently clear their generated and compared coverage floors;
- cross-model inclusion: 1,723 complete programs and 17,230 per-kind checks,
  zero global skips, and all 4 of 4 dedicated TryLock programs compared with
  zero TryLock skips.

The optimality meter corpora are intentionally unchanged. Their exhaustive
action rendering/key paths understand `TryLock`, but the pinned meter lines
remain byte-identical at SC 1.067, TSO 1.152, and PSO 1.154.

### Acceptance and Release cost

The explicit Release tree passed all 27 suites, and the Debug tree with
`DPOR_ENABLE_RESTORE_ASSERTS=ON` passed the same 27/27 in 262.17 seconds. The
default-off profiled tree also registers the classifier probe as CTest #28;
that focused test passed with both TryLock elision edges and exact history
restore enabled.

Release wall time was compared with a pristine `ae2b5a1` export built with the
same compiler and flags. Following ADR 0023, the complete CTest runs were
interleaved three times and the best result from each tree was compared to
reject transient host load:

| Release CTest | Run 1 | Run 2 | Run 3 | Best |
|---|---:|---:|---:|---:|
| Pristine `ae2b5a1` (26 suites) | 28.89s | 23.14s | 31.91s | 23.14s |
| TryLock campaign (27 suites) | 22.95s | 24.91s | 22.78s | 22.78s |

The campaign best is 0.36 seconds (1.6%) faster despite the additional
TryLock suite and widened corpora. This is within the expected host noise in
the favorable direction and establishes no Release wall-time regression. All
six timing runs passed their complete suite sets.

## Consequences

- Programs can build mutex-compatible test-and-set locks using ordinary result
  registers and branches, without a new blocking state or replay phase.
- Successful acquisition publishes exactly like `Lock`; failure cannot hide a
  race through a false synchronization edge.
- The third-holder reduction removes only the duplicate order of two guaranteed
  failures. Every winner, intervening release, mixed action, and owner-sensitive
  class remains conservative and oracle-checked.
- A pure retry loop is visibly non-terminating and fairness-classified rather
  than mislabeled as deadlock.
- Behavioral fingerprints remain compact because existing register and owner
  fields already encode the new observable outcomes.

## Invariants protected

- **Happens-before:** success joins exactly the mutex release frontier; failure
  joins nothing and changes no ownership or release state.
- **Independence soundness:** the local diamond requires exact co-enabled
  attempts and a third-party owner from the node snapshot, with persistent,
  sleep, repair, and occurrence safeguards audited explicitly.
- **Deadlock and lasso soundness:** attempts never block; backward retries use
  exact behavioral-state repetition and established weak-fairness labels.
- **Replay and values:** one attempt is one numeric source step and its exact
  `0`/`1` result is carried by the existing deterministic register state.
- **Namespace and weak-memory ordering:** TryLock is the same mutex resource as
  Lock/Unlock/Wait and is an explicit-drain ordered point under TSO and PSO.
- **Elision safety:** both outcomes advance pc, while every retry path contains
  the backward branch that activates exact cycle history.
