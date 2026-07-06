# ADR 0007: Atomic Operations

## Status

Accepted.

## Context

The IR needs atomic operations without weakening the naive oracle, DPOR
cross-validation, deterministic replay, or the happens-before soundness
invariants. The modeled memory-order subset is intentionally narrow:

- `AtomicLoad(address)` is acquire.
- `AtomicStore(address)` is release.
- `AtomicRmw(address)` is acquire-release.
- Atomics are sequentially consistent per location.
- No relaxed orderings are modeled.

This is not a full C++ memory model. It is a deterministic small-step model for
the checker, with enough release/acquire structure to validate DPOR and mixed
plain/atomic race detection.

## Decision

Maintain a per-address atomic location clock separate from plain-access race
metadata. Atomic operations are always enabled and do not introduce blocking,
modeled errors, or new replay schedule phases.

### Happens-Before Clock Semantics

Every executed action still ticks the executing thread clock before applying
the action semantics.

`AtomicStore(address)` is release. It replaces the location clock with the
storing thread's post-tick clock. It does not join the previous location clock
into the storing thread, and it does not join the previous location clock into
the new location clock. Joining here would fabricate happens-before edges from
earlier stores by other threads to later loads. Extra happens-before edges can
hide real plain-data races, which is unsound; missing edges only over-report.
Therefore this forced modeling choice errs toward fewer edges.

`AtomicLoad(address)` is acquire. It joins the loading thread clock with the
current location clock and does not mutate the location clock. This adds only
the synchronizes-with edge from the last store or release sequence represented
by this schedule. Omitting the join would over-report races after a real
release/acquire edge; joining any other clock would create extra edges that can
hide races.

`AtomicRmw(address)` is acquire-release and continues the C++ release sequence.
It first joins the executing thread clock with the current location clock, then
replaces the location clock with that now-joined thread clock. The join carries
the earlier release sequence forward; the replace avoids accumulating unrelated
previous stores from other threads. As with stores, the soundness bias is fewer
edges when in doubt, because extra edges suppress races.

### Race Semantics

The race detector records atomic accesses in each `AddressState` alongside the
existing plain-access metadata. It keeps the existing last-plain-write and
plain-reads-since-last-write path for plain/plain behavior, and adds ordered
vectors of plain and atomic accesses for the mixed plain/atomic check. The
ordered vectors preserve deterministic endpoint selection.

Race matrix for same-address accesses:

| Pair | Race when unordered by HB? |
| --- | --- |
| plain read / plain read | no |
| plain read / plain write | yes |
| plain write / plain write | yes |
| atomic / atomic, any combination | no |
| plain read / atomic load | no |
| plain read / atomic store | yes |
| plain read / atomic RMW | yes |
| plain write / atomic load | yes |
| plain write / atomic store | yes |
| plain write / atomic RMW | yes |

Different addresses do not race.

### Independence

The public `independent()` predicate remains conservative and action-only.
Same-address clauses are:

- Two `AtomicLoad` actions are independent. Neither mutates the location clock;
  both join the same clock into their own thread clock; state and enabledness
  commute.
- Any same-address atomic pair involving `AtomicStore` or `AtomicRmw` is
  dependent. The order changes the location clock and therefore downstream
  happens-before and mixed-race verdicts.
- Any same-address atomic/plain pair is dependent. The order can determine the
  mixed-race happens-before verdict.
- Plain/plain pairs keep the previous rule: conflicting accesses are
  dependent, and read/read is independent.
- Different-address atomic accesses are independent unless another existing
  mutex, condition-variable, or join clause makes the pair dependent.

### DPOR

Atomics add no blocking, so enabledness logic is unchanged except that the new
atomic action kinds are always enabled. `effective_next_action()` passes atomics
through unchanged; only `Wait` still needs phase-aware rewriting.

Sleep-set inheritance and happens-before-aware backtracking need no new
mechanism beyond the updated `independent()` and `may_conflict()` semantics.
Trace entries already store the effective action and post-step vector clock.
The atomic release/acquire edges are present in those vector clocks before DPOR
decides whether a dependent pair is happens-before ordered.

## Consequences

The naive oracle remains the reference semantics, and both oracle sweeps now
include `AtomicLoad(f)`, `AtomicStore(f)`, and `AtomicRmw(f)` in their
alphabets. Their assertions remain unchanged: DPOR must match naive race,
deadlock, and error verdicts; DPOR must not explore more schedules than naive;
and every DPOR report must replay identically.

This model can over-report relative to a richer C++ memory model when an
ordering is not represented. It must not under-report by adding speculative
happens-before edges.
