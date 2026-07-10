# ADR 0016: Exact State-Cycle Lasso Detection

## Status

Accepted.

## Context

ADR 0013 introduced backward branches and a deterministic per-thread step
bound. That bound prevents the checker itself from diverging, but it cannot
distinguish a genuinely repeating execution from one whose modeled state grows
forever. Reporting both as `clean up to bound` leaves a stronger fact unused:
if one schedule returns to exactly the same behavioral state, repeating the
same enabled cycle gives an infinite execution.

The dangerous error direction is a false proof. A hash collision that equates
two different states would fabricate non-termination. Cross-execution state
deduplication would also be unsound for this stateless DPOR design because it
could prune a different execution whose happens-before history exposes a
safety bug.

## Decision

### Exact behavioral fingerprint

The checker encodes behavioral state into a canonical byte string and compares
the complete strings. It does not use a digest or lossy hash. Integers use a
fixed-width little-endian encoding; strings and collections carry explicit
lengths; maps use their deterministic key order; thread-indexed vectors retain
thread order. Missing memory cells and explicit zero-valued cells are
canonicalized together because every modeled read observes zero in either
case. Empty condition-variable waiter entries are likewise canonicalized to
absence because signal and broadcast treat both identically.

Every `ExecutionState` field was audited:

| Field | Decision | Reason |
|---|---|---|
| `memory_model` | Include | Selects SC versus TSO enabledness and value semantics. |
| `pc` | Include | Determines each thread's next executable action. PCs are already normalized past labels. |
| `started` | Include | Determines which static thread bodies may execute and whether joins can finish. |
| `mutex_owner` | Include | Determines lock and woken-wait enabledness. |
| `mutex_clock` | Exclude | Vector-clock analysis instrumentation; it changes happens-before reports, not program control or values. |
| `condition_waiters` | Include | The ordered wait set determines signal/broadcast effects; empty entries are canonicalized away. |
| `thread_clock` | Exclude | Monotone happens-before instrumentation. |
| `registers` | Include | Branches, assertions, operands, and writes read register values. |
| `thread_steps` | Exclude | Exploration-budget accounting. Including it would make every loop appear non-repeating. |
| `wait_phase` | Include | Distinguishes sleeping waits from mutex-reacquire waits and changes enabledness. |
| `memory` | Exclude | `AddressState` contains race-analysis access history, not modeled values. |
| `memory_values` | Include | Reads, branches through loaded registers, CAS, and RMW depend on exact values; zero/absence is canonicalized. |
| `atomic_location_clock` | Exclude | Per-location vector-clock instrumentation. |
| `store_buffers` | Include | Under TSO, each FIFO address/value sequence changes forwarding, flush actions, ordered-point enabledness, and completion. |
| `schedule` | Exclude | Execution history, represented separately by the witness. |

Excluding vector clocks and race metadata is intentional but narrows the claim:
the witness proves that program behavior can repeat forever. It does not claim
that happens-before or race-analysis metadata repeats, and it does not replace
the safety reports accumulated before the cycle cut.

### Per-execution lasso detection

Each execution starts with a path-local map from the initial exact fingerprint
to schedule index zero. After every nonterminal transition, the checker
computes the next fingerprint:

- a new fingerprint is recorded with the current schedule length;
- a repeated fingerprint terminates that execution and records
  `NonTerminationReport { stem, cycle, schedule }`;
- `stem` ends at the first occurrence, `cycle` runs from that occurrence to the
  revisit, and `schedule` is exactly `stem + cycle`.

History is copied down a DFS path and discarded on backtrack. It is never
shared across sibling executions. `cycles_detected` counts cycle-cut execution
leaves; `first_nontermination` holds the first deterministic witness. Counts
can differ between naive and DPOR exploration.

The step bound remains a separate backstop. `bound_exceeded_executions` now
means that an execution exhausted its per-thread budget without first repeating
a behavioral state. Growing loops such as fetch-add-forever therefore remain
`clean up to bound` when no other bug or cycle exists. One exploration can find
both cycle leaves and residual bound leaves.

### Verdict and fairness semantics

Verdict priority is race, deadlock, modeled error, assertion, non-termination,
clean up to bound, then clean. Lower-priority report kinds remain visible through
`also_found` where applicable.

`nontermination` is a schedule-existence claim. An enabled peer may be
postponed forever by the witnessing schedule even when scheduling that peer
would allow termination. The result is not a fairness violation and makes no
fairness, starvation-freedom, or universal-termination claim.

### Replay and minimization

Replay runs the same exact, path-local comparison. Replaying `stem + cycle`
must reproduce the identical report, which validates equality between the
end-of-stem and end-of-cycle canonical states. A schedule that continues after
the first cycle closure is rejected with a deterministic invalid-schedule
error.

Lasso witnesses ship unminimized. The existing greedy minimizer can prove that
a deletion preserves a safety-report identity by replay, but arbitrary deletion
can move the first repeated state, destroy the cycle split, or produce a
different lasso. It therefore cannot honestly promise lasso preservation.
`minimize_schedule` returns a replayed non-termination witness unchanged.

### DPOR cycle cuts and agreement gates

A cycle cut is a terminal exploration leaf, like a bound outcome: it increments
the schedule count, stops that execution, clears sleep pruning at the parent,
and conservatively retains enabled siblings. There is one additional
sleep-set safeguard. The cycle-closing transition is not put to sleep across a
sibling transition, because the branch-first representative ended at the cut
and never executed the sibling afterward. Pruning the swapped sibling-first
prefix as though the full commute had been explored could lose a residual
bound, safety, or non-cycle continuation.

The class-representative claim is existence, not count equality. DPOR swaps
only cross-thread transitions whose modeled effects and enabledness commute. A
finite lasso prefix therefore remains represented either by a commuting class
representative that closes an equivalent behavioral cycle or by the
conservative sibling/backtrack handling at a cut. The no-sleep safeguard covers
the prefix-sensitive case where an independent swap delays the first observed
closure. Deterministic two-thread, three-thread, TSO, gallery, and fuzz gates
compare naive/DPOR cycle-existence booleans and replay the DPOR lasso exactly;
they do not require equal `cycles_detected` counts.

## Consequences

- Repeating spin schedules receive a replayable `nontermination` verdict
  instead of only a bounded verdict when a complete cycle fits within the
  configured budget.
- Mutual-exclusion safety remains evidenced separately by the absence of race
  and assertion reports; the lasso does not weaken or replace those checks.
- TSO fingerprints are larger because store-buffer contents are proof-critical.
- Exact strings cost more memory and comparison time than hashes, but avoid the
  one unacceptable outcome: a collision-based false divergence proof.
- Programs with genuinely growing state still rely on the explicit step bound.
