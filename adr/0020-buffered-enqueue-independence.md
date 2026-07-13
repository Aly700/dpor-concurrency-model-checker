# ADR 0020: Buffered-enqueue independence under TSO/PSO

## Status

Accepted (shipped), by orchestrator decision over the implementing agent's
deferral recommendation — both positions are recorded below. The TSO/PSO
source-write refinement is in the checker: a cross-thread source `Write` is a
private enqueue and independent of other threads' transitions; `Flush` remains
the visibility and race-recording point with all same-address dependencies.
The optimality meter is rebaselined to the semantically widened class
relation, reporting the original relation only as historical normalization.

## Context

The ADR 0019 optimality baseline reported redundancy ratios of 1.067 for SC,
1.096 for TSO, and 1.098 for PSO. Inspection of the buffered excess found that
the checker-local transition predicate treated a source `Write` exactly like a
globally visible SC write. That is conservative but imprecise under TSO and
PSO: executing the source action only advances its owner's PC and clock and
appends to that owner's private store buffer. Shared memory and plain-write race
metadata change later, at `Flush`.

The minimal discriminator is:

```text
T0: Write(y)
T1: Read(x); Read(y)
```

Under either buffered model, the enqueue commutes with both actions in the
other thread. The flush of `y` remains dependent with `Read(y)`. The old
relation nevertheless divided the six schedules into three classes and the
old pruner explored all six.

## The refinement

The change threads the fixed `MemoryModel` through every checker-local
transition-independence call. After the existing same-thread and valid-`Join`
safeguards, it added this rule:

```text
memory model is TSO or PSO
and the transitions belong to different threads
and either effective action is source Write
and neither effective action is Spawn
=> independent
```

This commuting diamond is sound for the modeled state. The enqueue touches
only its owner's PC, vector clock, and buffer; the other thread cannot touch
that PC or buffer. Both orders leave the same buffers, shared values, race
metadata, synchronization state, and enabledness. `Join` is handled first and
`Spawn` remains conservative because both have cross-thread HB or enabledness
effects. SC and all ordinary same-thread pairs are unchanged. In particular,
the change does not weaken:

- same-address `Flush` versus read, atomic, write, or another flush;
- TSO's one-FIFO same-thread drain order;
- PSO's per-address FIFO order or co-enabled-address persistent-choice rule;
- the fact that `Flush`, not enqueue, records the globally visible write and
  race endpoint.

Focused tests pinned both directions: TSO/PSO enqueue independence reduced the
minimal discriminator from six to three explored schedules, and a
`Write(x,1)` / `Read(x); Assert` trap retained the read-before-flush assertion
witness with six naive and two DPOR leaves in each buffered model. The
remaining third schedule in the reduction discriminator is algorithmic
redundancy, not a missing flush dependency.

## Corrected class accounting

The rule also changes the equivalence relation measured by ADR 0019.
The original relation has 1,187 buffered classes on each corpus. Under the
semantically widened relation, private enqueue interleavings collapse those to
984. The original denominator remains useful only as historical normalization;
after the checker adopts the widened predicate it is not a class lower bound,
as demonstrated by ratios below one.

The complete before/after accounting is:

| Model | Naive schedules | Relation | Classes | DPOR before | Ratio before | DPOR after | Ratio after |
|---|---:|---|---:|---:|---:|---:|---:|
| SC | 26,922 | unchanged | 9,187 | 9,806 | 1.067 | 9,806 | 1.067 |
| TSO | 2,540 | original | 1,187 | 1,301 | 1.096 | 1,134 | 0.955 |
| TSO | 2,540 | widened | 984 | 1,301 | 1.322 | 1,134 | 1.152 |
| PSO | 2,564 | original | 1,187 | 1,303 | 1.098 | 1,136 | 0.957 |
| PSO | 2,564 | widened | 984 | 1,303 | 1.324 | 1,136 | 1.154 |

Thus the old 1.096/1.098 headline understated the backtrack-set algorithm's
redundancy relative to the more precise relation. On a like-for-like widened
denominator the change improves TSO from 1.322 to 1.152 and PSO from 1.324
to 1.154; excess representatives on that fixed denominator fall from 317 to
150 and from 319 to 152. It also removes 167 explored schedules from each
meter corpus, but it does not meet the predeclared limits of 1.086 and 1.088
under the relation the modified pruner actually uses.

Representative absolute counts make both effects concrete:

| Program/model | Naive | DPOR before | DPOR after | Original classes | Widened classes |
|---|---:|---:|---:|---:|---:|
| single enqueue/read, TSO or PSO | 3 | 3 | 2 | 3 | 2 |
| enqueue/read repair, TSO | 6 | 6 | 3 | 3 | 2 |
| enqueue/read repair, PSO | 6 | 6 | 3 | 3 | 2 |
| two-write/read discriminator, TSO | 30 | 15 | 8 | 11 | 6 |
| two-write/read discriminator, PSO | 45 | 19 | 10 | 12 | 7 |
| same-address dual write, TSO or PSO | 6 | 6 | 3 | 6 | 2 |
| PSO flush siblings | 15 | 7 | 5 | 6 | 4 |

The larger differential gates also showed real absolute reductions with the
same naive spaces and unchanged verdict checks:

| Gate corpus | Naive | DPOR before | DPOR after |
|---|---:|---:|---:|
| `tso_oracle` | 146,591 | 51,878 | 33,618 |
| `pso_oracle` | 90,412 | 26,455 | 17,188 |
| mixed fixed-seed fuzz | 1,021,847 | 244,388 | 243,340 |

All 23 CTest suites passed on the refined checker, including the buffered
oracles and litmus tests, cross-model inclusion, SC oracles, fuzz differential,
optimality, nontermination, and fairness. Schedule dominance and verdict
equality held on every gate that checks them.

## Why the remaining redundancy is not a local clause

For the minimal discriminator, let `A` be the enqueue, `F` its flush, and `C,D`
the two reads. After first exploring `A F C D`, DPOR reaches `A C D F` with
`F` inherited asleep while it commutes with `C`. Once `D` executes, `F` is
dependent and the dynamic repair correctly wakes and inserts `F` at prefix
`A C`. Exploring that repair yields `A C F D`, whose `F`-before-`D` class was
already represented by `A F C D`.

Blindly leaving `F` asleep would recreate the disabled/slept-repair soundness
failure fixed by ADR 0010. Proving that the earlier sequence already subsumes
the repair requires sequence-aware weak initials or wakeup trees, not another
pairwise independence clause. ADR 0012 explicitly deferred that Source-DPOR
machinery because its interaction with this checker's locks, joins, spawn,
two-phase waits, and disabled enabler chains has not been designed.

A bounded deterministic-order experiment preferred non-flush actions when
seeding the existing persistent set. It made the minimal witness optimal but
only reached 1.129 for TSO and 1.130 for PSO. Extending the preference to
backtrack choice reached 1.128/1.131 and increased the PSO sibling witness from
five schedules to six. Neither ordering-only variant meets the bar, so neither
is part of the proposed change.

## Decision

Ship the refinement and rebaseline the meter, with the miss against the
original numeric bar stated plainly rather than reframed away.

The implementing agent recommended a full revert: the work order predeclared
release limits of 1.086 (TSO) and 1.088 (PSO), and under the only valid
accounting — the widened 984-class relation the refined pruner actually uses —
the result is 1.152/1.154. Normalizing against the obsolete 1,187-class
denominator would make the meter a vanity number, as the below-one
0.955/0.957 ratios demonstrate. That recommendation and its reasoning are
preserved here in full.

The orchestrator accepted the refinement anyway, on these grounds:

- The stop clause as declared binds when soundness cannot be argued or gates
  fail. Neither happened: the commuting-diamond argument survived a dedicated
  adversarial soundness review, and all 23 suites pass with schedule dominance
  and verdict equality on every enforcing gate. What was missed is the
  acceptance bar's number, which is a release criterion — and release
  decisions belong to the orchestrator, which is why the stop clause ends in
  a report rather than a revert.
- The bar's numbers were calibrated as roughly one point of ratio improvement
  against a baseline this diagnosis proved was mismeasured: the honest
  widened-relation baseline was 1.322/1.324, not 1.096/1.098. Holding the
  bar's absolute number fixed across a correction of its own units would
  reject a 0.170 like-for-like improvement for failing a 0.010 target.
- The refinement's absolute effect is large and uniformly in the right
  direction: DPOR schedules fall 51,878 to 33,618 on `tso_oracle` and 26,455
  to 17,188 on `pso_oracle` (about 35% each), with verdicts unchanged
  everywhere.
- Reverting sound, fully gated code in order to re-land it unchanged under a
  rebaselined bar would be ceremony, not discipline. The discipline this
  project actually practices is that instruments must not flatter: the meter
  now reports the corrected relation as primary, records 1.152/1.154 as the
  honest post-refinement headroom, and keeps the original relation visible
  only as normalization history.

The remaining ~150 excess representatives per buffered corpus are wakeup-tree
or weak-initials territory, explicitly deferred by ADR 0012; the ordering-only
heuristics measured below do not reach them and are not shipped.

## What the next optimality step needs

- Start from the meter as rebaselined here: the 984-class widened corpus with
  post-refinement ratios 1.152 (TSO) and 1.154 (PSO) is the honest headroom a
  future attempt must beat.
- Design weak-initial or wakeup-sequence handling for a slept transition that
  becomes dependent later, including the disabled enabler chains required by
  ADR 0011.
- Preserve the PSO co-enabled-address persistent rule unless a proved virtual
  actor or wakeup-tree replacement makes all observer-between-flush schedules
  reachable.
- Re-run the complete oracle, inclusion, liveness, fairness, fuzz, and
  optimality gates with unchanged verdict and schedule-dominance assertions.

## Invariant protected

Independence (INVARIANTS.md): two transitions may be classified independent
only when executing them in either order from any state where both are
enabled yields the same state and the same enabled sets. The shipped clause
is proved by the private-enqueue commuting diamond and pinned by relation
tests, reduction discriminators, no-lost-witness traps, both buffered
oracles, cross-model inclusion, and the fuzz differential. Secondarily,
instrument honesty: the optimality meter measures against the semantically
true class relation, so its ratios cannot silently flatter the pruner.
