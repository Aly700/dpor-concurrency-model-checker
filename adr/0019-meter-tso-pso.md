# ADR 0019: TSO/PSO DPOR Optimality Meter

## Status

Accepted.

## Context

ADR 0014 established an SC-only quantitative gate between the exhaustive
schedule count and the DPOR schedule count. ADR 0015 left buffered-model
optimality unmeasured because internal flushes require a transition alphabet
and same-thread correspondence different from source-program order. ADR 0017
then made the missing PSO rule concrete: two flushes owned by one thread may
commute when they drain different address FIFOs, even though ordinary source
actions from one thread cannot reorder.

The TSO and PSO differential oracles prove soundness, but they do not say how
many redundant representatives flush handling explores. The meter therefore
needs to cover all three memory models without changing the pruner or
inflating the class minimum with an extra same-thread edge.

## Decision

### Observational transition identity

The meter continues to use only the read-only verification surface from ADR
0014. `replay_effective_trace()` already exposes both parts of buffered flush
identity:

- the schedule endpoint retains `kFlushActionIndex`; under PSO it also retains
  the canonical numeric `flush_address` id;
- the phase-aware effective action is `Flush(concrete_address)` under both TSO
  and PSO.

Transition labels include the effective action address and a per-transition
occurrence number. PSO drains of different addresses are therefore distinct,
and repeated drains of one address remain deterministic. Cross-thread
dependence still calls `dpor_transitions_independent()`, the same observational
wrapper around the predicate used by the pruner. No exploration state,
backtrack choice, sleep set, or transition predicate was changed for this
extension.

### Same-thread flush DAG correspondence

For two trace positions `i < j`, the dependence DAG encodes exactly the
same-thread order that a legal schedule and the pruner's equivalence may use:

1. Same-thread program-ordered action steps are **ordered**. A pair containing
   one source action and one internal flush is also ordered; only a pair of
   internal flushes can take the PSO exception below.
2. TSO same-thread flushes are **ordered**. Its store buffer is one FIFO, so
   later drains cannot pass earlier drains.
3. PSO same-thread flushes of the same address are **ordered**. Each address
   queue is FIFO.
4. PSO same-thread flushes of different addresses are **unordered** directly.
   They receive no same-thread DAG edge when the transition predicate says
   they commute. Another dependence path may still order them; the
   canonicalizer preserves every such path.

All cross-thread pairs use the existing transition predicate: flushes are
write-like and dependent with another thread's memory action or flush on the
same address, and independent of different-address memory or register-only
work. The checker-local enabled-valid-`Join` refinement remains part of that
predicate.

An executable discriminator pins the direction. For
`T0: write(x); write(y)` and `T1: read(y); read(x)`, TSO has 11 classes and PSO
has 12. The additional PSO class requires the legal `flush(y); flush(x)` drain
order. Treating every same-thread pair as ordered would erase that distinction
and overstate the PSO class minimum.

### Scope

A program is metered under a model only when exhaustive naive exploration:

- is not schedule-capped;
- has at most 24 maximal schedules for enumerated and fuzz sources;
- reports `cycles_detected == 0`;
- reports `bound_exceeded_executions == 0`; and
- has no modeled error or assertion execution.

The explicit cycle count matters even when another higher-priority verdict is
also present. Lasso detection terminates a repeating execution at its first
exact state revisit; that schedule is a cycle witness prefix, not a maximal
finite schedule. Likewise, a bound-hit leaf is a truncated prefix. Including
either in a linearization count would compare DPOR against something other
than maximal Mazurkiewicz trace classes. Error and assertion programs retain
ADR 0014's exclusion because bug-existence pruning may keep a shorter terminal
representative after omitting an independent prelude.

The buffered candidate probe stops at schedule 25, which is sufficient to
reject an over-limit space without exhausting it. Every eligible space is then
collected and replayed in full.

### Deterministic corpora

The SC corpus and its generators are unchanged. TSO and PSO each use the
common 11-action two-thread family from their oracle gates, restricted to
thread lengths zero through two and at most 128 deterministic strided samples
per length pair. Each model also receives 128 candidates from the fixed seed
`0x7b83d52fa9614c0d`; the fuzz alphabet adds register-only, yield, and
assertion actions to the buffered oracle actions. Eligibility applies after
generation, so excluded programs do not contribute to the counts below.

| Model | Enumerated family | Hand-picked | Fixed-seed fuzz | Total metered |
|---|---:|---:|---:|---:|
| SC | 7,120 | 7 | 35 | 7,162 |
| TSO | 600 | 0 | 67 | 667 |
| PSO | 600 | 0 | 67 | 667 |

The TSO/PSO class discriminator is an exact contract test, not part of either
aggregate corpus.

## Measurements

Release baseline on July 10, 2026:

```text
dpor_optimality: programs metered=7162 total_classes=9187 total_dpor_schedules=9806 total_naive_schedules=26922 redundancy_ratio=1.067 optimal_programs_percent=93.4 source_two_thread=7120 source_hand_picked=7 source_fuzz=35 within_class_same_verdict=held
dpor_optimality_tso: programs metered=667 total_classes=1187 total_dpor_schedules=1301 total_naive_schedules=2540 redundancy_ratio=1.096 optimal_programs_percent=92.5 source_enumerated=600 source_fuzz=67 within_class_same_verdict=held
dpor_optimality_pso: programs metered=667 total_classes=1187 total_dpor_schedules=1303 total_naive_schedules=2564 redundancy_ratio=1.098 optimal_programs_percent=92.2 source_enumerated=600 source_fuzz=67 within_class_same_verdict=held
```

The SC line is byte-identical to the ADR 0014 baseline. Within-class public
verdict kind remained identical for every TSO and PSO class.

TSO executes 114 schedules above its measured class minimum; PSO executes
116. Their redundancy ratios are slightly worse than SC's 1.067. This is new
quantitative information about internal flush handling and records headroom
for a future flush-aware reduction design. It is not a reason to tune or
weaken the current conservative pruner.

## Consequences

- The optimality gate now reports SC, TSO, and PSO independently.
- A future class-count change must preserve the four same-thread rules above;
  especially, adding an unconditional edge between different-address PSO
  flushes is a meter regression even if all soundness oracles remain green.
- Buffered-model reductions can now be evaluated against a measured minimum
  without treating a worse-than-SC redundancy ratio as a correctness failure.
- ADR 0015's statement that the meter remains SC-only is superseded by this
  decision.
