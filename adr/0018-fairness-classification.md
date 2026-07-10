# ADR 0018: Weak-Fairness Classification of Lasso Witnesses

## Status

Accepted.

## Context

ADR 0016 proves schedule-existence of divergence by returning an exact lasso,
but the original report did not distinguish a spinner that indefinitely
postpones a runnable peer from a cycle that remains possible under a weakly
fair scheduler. Those witnesses have different diagnostic quality even though
both are valid non-termination proofs.

This distinction must not be overstated. **The classification describes this
reported witness only. It is not a system-level liveness verdict.** Breaking an
unfair witness by scheduling its postponed peer may reach a different fair or
unfair cycle. Likewise, reporting the first fair-divergence witness does not
prove that every divergent execution is fair.

## Decision

### Two witness classes

For a reported cycle with states `s0, s1, ..., sn`, where `sn == s0`, let `P`
be the set of thread IDs owning at least one scheduled transition in the cycle.
A flush transition is owned by its source thread and therefore makes that
thread a participant.

For every `t` not in `P`, the checker replays the exact witness and evaluates
thread-level enabledness at the cycle start and after every cycle transition.
`t` is enabled in a state when it has at least one enabled source transition or
internal flush transition. Under TSO a nonempty thread FIFO enables its flush;
under PSO every nonempty per-address FIFO enables a distinct flush. Finished,
unstarted, mutex-blocked, join-blocked, and sleeping condition-variable threads
are disabled according to the existing interpreter rules.

The report field is:

```cpp
enum class Fairness { UnfairScheduleWitness, FairDivergence };
```

- If some `t` not in `P` is enabled in every cycle state, the report is
  `UnfairScheduleWitness`. Weak fairness would eventually schedule `t`, so this
  cycle as witnessed cannot persist.
- Otherwise, every non-participant is disabled in at least one cycle state and
  the report is `FairDivergence`. The cycle can repeat without violating weak
  scheduler fairness. This includes cycles in which every unfinished thread
  participates and cycles whose only non-participants remain blocked.

There is deliberately no “productive” sub-class. Exact state repetition makes
PC-net-zero and behavioral-state-net-zero automatic. Labels such as productive
or progress-free would add interpretation without strengthening the proof.

### Weak, not strong, fairness

The criterion is weak fairness: a continuously enabled thread must eventually
be scheduled. A thread enabled in some cycle states but disabled in another is
not protected. Strong fairness would additionally protect a thread enabled
infinitely often, even if not continuously; classifying that property would
require a separate report field and is future work.

### Detection and replay

Cycle observation records only the exact first-visit schedule index. Report
construction then deterministically replays `stem + cycle` from the initial
state, derives `P` from the cycle schedule, and intersects non-participant
enabledness across the reconstructed cycle states. The helper also rechecks
that the end-of-cycle behavioral fingerprint equals the end-of-stem
fingerprint. Both naive and DPOR detection, and public `replay()`, use this one
constructor; report equality therefore validates the fairness field as well as
the stem, cycle, and schedule.

`first_nontermination` remains first-found. It is not replaced by a later fair
witness because that would change deterministic witness selection and existing
schedules. `CheckResult::fair_cycles` and `unfair_cycles` count all cycle-cut
leaves by class, so callers can ask whether either class exists in the explored
space. The invariant is:

```text
fair_cycles + unfair_cycles == cycles_detected
```

An unfair first report does not imply that no fair-divergence witness exists
elsewhere, and a capped exploration cannot prove either class absent.

### DPOR class-existence agreement

The reduction gate compares existence of each class, not raw class counts or
the class of the first witness. Commuting independent transitions preserves
the transition owners and each thread's transition sequence, so `P` is stable.
It also preserves the same exact cycle-boundary state. For enabledness inside
the cycle, the modeled blockers are concrete state: mutex ownership, target
completion/start, wait phase and wake set, or the thread's own pending buffers.
Two transitions that change the same blocker are dependent (and PSO keeps all
co-enabled same-owner address flush choices), so DPOR does not justify pruning
their ordering as an invisible commute.

This is a design argument, not a formal proof that every linearization of a
Mazurkiewicz class has the same fairness label: commutation can change interior
states and can change where the first exact cycle closes. The honest executable
claim is therefore existence-of-class agreement over complete explorations.
`dpor_oracle`, `tso_oracle`, `pso_oracle`, and deterministic differential fuzz
all require equality of `fair_cycles > 0` and `unfair_cycles > 0` between naive
and DPOR. The positive oracle fixtures contain both classes, so neither check
is vacuous. Raw counts remain diagnostic and may differ because DPOR explores
representatives.

The gates held without weakening. If a future model extension makes an
interior-state distinction violate existence agreement, the implementation or
independence relation must be fixed. A weaker fuzz-observed claim must not be
substituted silently; it would require a new ADR explaining the limitation.

## Consequences

- Every non-termination report explains whether its exact witness survives
  weak scheduler fairness.
- Flush ownership and flush enabledness use the same interpreter semantics as
  execution and replay; no parallel fairness-specific model exists.
- Classification adds deterministic replay work at each detected cycle. This
  favors a narrow correctness path over retaining full state snapshots in the
  path-local fingerprint map.
- First-report output remains stable except for the added `fairness` line.
- System-level liveness under fairness and strong-fairness classification remain
  explicitly out of scope.
