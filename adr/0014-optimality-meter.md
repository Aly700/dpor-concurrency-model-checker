# ADR 0014: DPOR Optimality Meter

## Status

Accepted.

## Context

ADR 0012 deferred Source-DPOR with wakeup trees because the shipped checker is
still a backtrack-set DPOR with sleep sets and enabler-chain repairs. The three
existing verification gates prove soundness against the naive oracle, but they
do not quantify how far `explore_dpor` is from the theoretical target of one
representative per Mazurkiewicz trace class.

This ADR adds that quantitative gate. It is an acceptance instrument for any
future wakeup-tree work: an optimal implementation should drive DPOR schedules
toward the class count without weakening the existing oracle comparisons.

## Decision

`tests/dpor_optimality.cpp` enumerates maximal schedules with a new read-only
`ModelChecker::collect_naive_schedules()` helper. The helper is separate from
`explore_naive`; it reuses the same interpreter, enabledness, terminal error,
assertion, deadlock, and step-bound semantics, but records terminal schedules
instead of only counting them.

For each collected schedule, the meter calls
`ModelChecker::replay_effective_trace()`. This read-only replay observation
returns the phase-aware effective action for every executed schedule step. In
particular, `Wait(cv, mutex)` appears as the static `Wait` during its
release-and-sleep phase and as an effective `Lock(mutex)` during its later
woken reacquire phase, matching ADR 0006 and the DPOR trace entries.

The meter builds a dependence DAG over the effective trace. For each pair
`i < j`, it adds an edge when the steps are in the same thread or when
`ModelChecker::dpor_transitions_independent()` says the two transitions do not
commute. That helper is intentionally the same transition predicate used by the
pruner: the public action-level `independent()` relation for ordinary pairs,
plus the checker-local enabled-valid-`Join(target)` refinement from ADR 0011.
Using raw action-level `independent()` would measure against a stricter
relation than the current pruner is allowed to exploit for joins, so the class
count would stop being a lower bound for this checker.

The canonical trace-class identity is the lexicographically minimal
topological linearization of that DAG. Vertex labels include thread id, action
index, effective action fields, and a per-transition occurrence number so that
looped or repeated endpoints remain deterministic without using schedule
position as identity. Kahn's algorithm with the smallest available label at
each step gives a deterministic canonical form for every collected schedule.
The number of distinct canonical forms is the measured class count.

For every canonical class, the meter also replays every member schedule and
checks that all schedules in the class have the same public verdict kind:
`race`, `deadlock`, `error`, `assertion`, `clean up to bound`, or `clean`, using
the same priority order as the CLI report. A violation is a soundness finding
against the independence relation: it would mean a supposedly commuting swap
can change the modeled verdict.

## Scope

The gate meters programs whose naive schedule space is exhausted, whose
executions do not hit the step bound, and whose public naive verdict has no
modeled error or assertion. Terminal modeled errors and assertions are excluded
from this baseline because the current checker is a bug-existence verifier: it
can intentionally keep a shorter terminal report while pruning independent
prelude work that would produce the same terminal verdict. Those invalid
programs are still covered by the three existing differential gates; the
optimality meter focuses on clean, race, and deadlock trace classes where
`class_count <= dpor <= naive` is the expected shape.

The enumerated two-thread and fuzz sources are additionally filtered to
programs with at most 24 naive schedules so the suite remains a small,
deterministic Release-mode gate. The hand-picked exact-count and
strict-reduction fixtures are all metered even when their naive schedule count
is larger.

## Measurements

Baseline on July 7, 2026:

```text
dpor_optimality: programs metered=7162 total_classes=9187 total_dpor_schedules=9806 total_naive_schedules=26922 redundancy_ratio=1.067 optimal_programs_percent=93.4 source_two_thread=7120 source_hand_picked=7 source_fuzz=35 within_class_same_verdict=held
```

Program-source counts:

- 2-thread family after filtering: 7,120 programs.
- Hand-picked exact-count and strict-reduction fixtures: 7 programs.
- Fixed-seed fuzz sample after filtering: 35 programs.

The current algorithm is close but not optimal on this corpus:
`explore_dpor` executes 619 schedules above the measured class minimum
(`9,806 - 9,187`), with an aggregate redundancy ratio of `1.067`. 93.4% of
metered programs are already optimal (`dpor == classes`). This quantified gap
is the baseline that wakeup trees should reduce.

The within-class same-verdict property held for every metered program in the
baseline run.

## Consequences

The checker now has a fourth deterministic verification gate. Future reduction
work must report this line alongside the existing oracle sweeps and fuzz gate.
Improving schedule counts without reducing `total_dpor_schedules /
total_classes` is not evidence of progress toward optimal DPOR.

The new core API surface is intentionally observational only:

- `collect_naive_schedules()`
- `replay_effective_trace()`
- `dpor_transitions_independent()`

These helpers expose what the interpreter and pruner already compute; they do
not mutate or steer exploration.

## Consequence for ADR 0012 (wakeup trees)

The meter answers the question ADR 0012 left open. Measured baseline:
redundancy ratio 1.067 with 93.4% of metered programs already optimal
(7,162 programs; 9,187 classes; 9,806 DPOR schedules). Wakeup trees — the
highest-risk rewrite this project could attempt, in the exact code region
where both historical soundness bugs lived — would buy at most 6.7% on
metered scales. The attempt remains closed, now by instrument rather than
by estimate. Revisit only if a metered workload shows a materially larger
gap.
