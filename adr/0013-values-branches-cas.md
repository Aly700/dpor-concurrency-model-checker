# ADR 0013: Values, Branches, CAS, Assertions, and Step Bounds

## Status

Accepted.

## Context

The original IR had fixed per-thread control flow and value-less memory
accesses. That was enough to validate DPOR, happens-before race detection,
locks, condition variables, spawn, join, and atomics, but it could not model
Peterson/Dekker-style algorithms or lock-free code whose control flow depends
on loaded values.

The extension must preserve the existing replay invariant: public schedules
remain deterministic `(thread, action_index)` pairs. It must also avoid adding
speculative happens-before edges, because this project has explicit atomics
tests showing that extra edges can hide real races.

## Decision

Values are signed 64-bit integers. Every thread has eight thread-local
registers, named `r0` through `r7`, initialized to zero. Register state is not
shared state.

The new action forms are:

- `set rN IMM`
- `label NAME` and line-label `NAME:`
- `bnz rN NAME`
- `assert rN`
- `read x -> rN` and legacy `read x`
- `write x IMM|rN` and legacy `write x`
- `atomic_load f -> rN` and legacy `atomic_load f`
- `atomic_store f IMM|rN` and legacy `atomic_store f`
- `atomic_rmw f IMM|rN -> rN` and legacy `atomic_rmw f`
- `cas f EXPECTED_IMM|rN NEW_IMM|rN -> rN`

Legacy value-less reads and loads discard the loaded value. Legacy writes and
stores write `0`. Legacy `atomic_rmw f` is fetch-add by `1` with a discarded
result. These defaults keep existing `.dpor` programs valid and do not change
their synchronization behavior.

Labels are pseudo-actions stored in the program vector so action indexes remain
stable, but they are never scheduled. The interpreter normalizes a thread's
program counter past labels at startup, after branches, after normal advances,
and after spawn starts a target. `ScheduleStep.action_index` remains the pc of
the real action that executed. Replay still checks `step.action_index ==
current_pc` after this label normalization.

`bnz rN label` changes only its own thread's pc. It jumps when `rN != 0`,
otherwise it advances to the next executable action. Branch targets must be
labels in the same thread in the CLI format; direct IR programs with a missing
label report a modeled error at the branch endpoint.

Plain and atomic memory cells share the existing address namespace and hold
one int64 value initialized to zero. Plain reads read the current schedule-order
cell value and write it to the destination register when one is present. Plain
writes evaluate their immediate/register operand and update the cell. This is
not a weak-memory value model: racy reads observe whatever the explored
interleaving produced, and the race report is the bug.

Atomic loads read the same value cell and keep the existing acquire clock
semantics. Atomic stores update the value cell and keep the existing release
store-replaces-location-clock semantics. `atomic_rmw` is fetch-add: it returns
the old value in its destination register, adds the operand to the cell, and
keeps the existing acquire-release RMW clock semantics.

`cas` is compare-and-swap on an atomic address. It reads the current value and
compares it with the expected operand. On success it stores the desired operand
and writes `1` to the result register. On failure it leaves the cell unchanged
and writes `0`. A successful CAS is modeled as the same acquire-release RMW
clock operation as `AtomicRmw`: join the location clock, then replace it with
the joined thread clock. A failed CAS is modeled as acquire load only: join the
location clock and do not replace it. This asymmetry is required because
replacing the location clock on failure would publish the failing thread's
prior plain accesses to later acquire loads and can hide real races.

`assert rN` reports `AssertionFailure { endpoint, register, value, schedule }`
when `rN == 0`. Assertion failures are terminal execution reports like modeled
errors, but they are a distinct first-class result kind and minimize through
replay identity.

Backward branches make executions unbounded. `ModelChecker` therefore has a
deterministic per-thread step bound, defaulting to 2000 steps per thread per
execution and configurable through the constructor and CLI `--step-bound N`.
If a schedule next chooses a thread that has already executed its bound number
of steps, that execution terminates with a bound outcome. `CheckResult` records
`bound_exceeded_executions` as a count. A clean result with a nonzero count is
reported by the CLI as `verdict: clean up to bound`, not plain `clean`.

## Independence and DPOR

Register-only actions (`Set`, `BranchNonzero`, `Assert`, and labels, though
labels are never scheduled) are independent of every action in another thread:
they touch no shared modeled state and do not change another thread's
enabledness. Same-thread transitions remain non-commutable by the DPOR
transition wrapper.

Value-carrying memory operations keep the existing address-based dependence
rules. Values do not weaken dependence. Two same-address writes of the same
value are still dependent because their order can change race endpoints, value
observations, and future happens-before state.

CAS is treated as an atomic RMW by the public action-level dependence predicate
regardless of whether a particular execution succeeds. Runtime success can
change the value cell and the location clock, and the action-only predicate
must be conservative.

Bound outcomes are compared between naive and DPOR only as a boolean. Counts
can legitimately differ because DPOR explores representatives rather than every
schedule. The boolean is preserved by the same independence argument used for
ordinary representative schedules: actions pruned into one Mazurkiewicz trace
class are related only by cross-thread swaps of independent transitions, so the
per-thread sequence length of every class member is the same. Therefore a class
representative hits the per-thread bound iff the class members hit it. Terminal
bound choices clear sleep pruning and add enabled siblings, matching the
existing conservative handling for terminal errors and assertions.

## Consequences

The naive oracle remains the reference semantics. The three differential gates
now compare race, deadlock, modeled-error, assertion, and bound-hit-boolean
existence, while keeping DPOR schedule dominance and replay identity checks.

This model is sound only relative to the configured step bound. A clean bounded
run proves absence of modeled bugs only up to that bound; a nonzero bound count
means the checker reached at least one execution whose unbounded continuation
was not explored.
