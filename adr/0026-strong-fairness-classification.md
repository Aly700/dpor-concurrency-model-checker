# ADR 0026: Strong-fairness classification of lasso witnesses

## Status

Accepted.

## Context

ADR 0018 divides exact lasso witnesses using weak scheduler fairness. A cycle
is an `unfair-schedule witness` when a thread that owns no cycle step remains
enabled at every replayed cycle state. Otherwise it was called
`fair divergence`.

That second bucket conflated two different witness schedules. A
non-participant can be disabled at one cycle state, enabled at another, and
never selected. Repeating the exact cycle enables it infinitely often, so the
witness violates strong fairness even though it satisfies weak fairness. The
smallest discriminator is a mutex owner that repeatedly unlocks and retakes
the mutex while another thread's pending `Lock` endpoint is enabled only in
the released state.

The classification remains diagnostic witness post-processing. It must not
change exploration, first-witness selection, lasso boundaries, or the legacy
weak label. In particular, every existing `unfair-schedule witness` byte must
remain unchanged.

## Decision

### Three classes with strongest-label selection

Let the replayed cycle states be `s0, s1, ..., sn`, where `sn == s0`. Let `P`
be the set of thread IDs that own a source or flush step scheduled in the
cycle. This deliberately preserves ADR 0018's participant definition.

For each `t` not in `P`, the checker asks the interpreter for the exact enabled
schedule endpoints at every state. Endpoint identity is the public replay
identity:

```text
(thread, action_index)                 source and TSO flush
(thread, action_index, flush_address)  PSO flush
```

The PSO address is its canonical numeric program-address ID. `Wait` sleep and
reacquire phases intentionally share their public source endpoint, and barrier
generation is intentionally not part of a schedule endpoint. Those choices
match replay identity rather than inventing a fairness-only transition model.

The report applies this strict precedence:

1. `unfair-schedule witness`: some `t` outside `P` has at least one enabled
   endpoint at every cycle state. This is ADR 0018's weak predicate verbatim.
2. `strongly-unfair-schedule witness`: the weak predicate is false, but at
   least one exact endpoint owned by a thread outside `P` is enabled in at
   least one cycle state. Because the cycle repeats, that endpoint is enabled
   infinitely often and never taken by this witness.
3. `fair divergence`: no endpoint owned by any thread outside `P` is enabled
   anywhere in the cycle.

Every weakly unfair witness is also strongly unfair in the logical sense, but
the first label is reported because it is the strongest applicable diagnosis.
The legacy spelling is unchanged. The only new output is:

```text
nontermination:
  fairness: strongly-unfair-schedule witness
```

The public enum explicitly retains the legacy underlying values
`UnfairScheduleWitness == 0` and `FairDivergence == 1`; the new enumerator is
appended logically as value 2. Likewise, `strongly_unfair_cycles` is appended
to `CheckResult` rather than inserted among its legacy aggregate members, so
existing positional aggregate initialization keeps its old meaning.

### Backward-compatible weak predicate

The implementation keeps ADR 0018's `continuously_enabled[tid]` calculation
as the authority for `unfair-schedule witness`: at each replayed state it asks
whether that thread has some enabled source or flush step. It does not replace
that predicate with intersection of endpoint identities, even though exact
cycle closure makes them coincide for current actions. This avoids making
backward compatibility depend on a new proof about endpoint stability.

Alongside that old calculation, an ordered set unions every exact enabled
endpoint observed for non-participants. A false weak predicate plus a nonempty
union selects the new class. An empty union selects fair divergence. The
closing state is observed as well as the cycle start and every successor; its
duplication cannot change either predicate and continues to validate exact
closure.

### Scope: witness schedules and non-participant owners

This classifies **the reported witness schedule, not the program**. Breaking a
strongly unfair witness by scheduling its blinking peer can terminate, expose
a bug, or reach a different fair or unfair cycle. A first witness of any class
does not prove the other classes absent.

The compatibility boundary is also intentional: once a thread owns any cycle
step, all of that thread's endpoints are excluded from fairness classification,
as in ADR 0018. This is scheduler fairness between modeled threads, refined
with exact endpoint enabledness for non-participants. It is not a claim of
action fairness among several endpoints owned by one participating thread. For
example, a participating thread that repeatedly takes source steps while
postponing its own pending flush is outside this report field's scope.

### Determinism and cost

Classification remains a pure function of `program`, `memory_model`,
`stem + cycle`, and interpreter enabledness. Replay reconstructs the same
states and uses ordered vectors and `std::set<ScheduleStep>`; there is no clock,
randomness, unordered iteration, or new search.

The extra work occurs only inside `make_nontermination_report()` after an
exact cycle has already closed. Programs without lassos perform no new
classification work and explore exactly the same schedules.

### Counters and agreement gates

`CheckResult` adds `strongly_unfair_cycles`. Every cycle-cut leaf increments
exactly one counter:

```text
fair_cycles + strongly_unfair_cycles + unfair_cycles == cycles_detected
```

`first_nontermination` remains first-found. Naive and DPOR gates compare
existence of all three classes independently, not raw counts or the class of
their possibly different first witnesses. The two-thread, three-thread, TSO,
PSO, classic-gallery, and generic cycle-capable helpers all enforce the
tri-state comparison. The deterministic fuzz differential compares the new
class, prints its raw counters, and runs a fixed uncapped blinking fixture so
the strong path remains executable even when the seeded random corpus happens
to contain none.

The optimality meter is unchanged. Its corpus excludes cycles, and fairness
post-processing does not alter the transition relation or schedule space.

## Fixtures

The focused fairness suite pins all three outcomes:

- **Weakly unfair:** a pure spinner postpones a continuously enabled peer;
  pending TSO/PSO flushes and the existing retained-holder `TryLock` retry
  remain legacy `unfair-schedule witness` cases.
- **Strongly unfair:** T0 owns `m`, then cycles through `Unlock(m)`, `Lock(m)`,
  and a backward branch. T1's exact `Lock(m)` endpoint is enabled only after
  the unlock and disabled after the reacquisition. The explicit witness cycle
  is T0 actions `3, 4, 5` and renders the new label.
- **Fair:** a finished peer and a peer permanently blocked on a mutex are never
  enabled in the cycle and remain `fair divergence`; a cycle in which every
  unfinished thread participates is also fair under this report scope.

The SC, TSO, and PSO oracles each include the mutex blink as a non-vacuous
positive strong-class fixture. The three-thread oracle includes both the
blink and a spinner/finite-peer fixture covering weak and fair existence.

## Golden and output audit

No committed golden file is reclassified. `tests/golden/` contains only the
data-race and AB-BA-deadlock reports, neither of which has a fairness field.

The existing byte-sensitive lasso outputs were audited individually:

- the single-participant nontermination report in `tests/nontermination.cpp`
  has no non-participant endpoint and remains `fair divergence`;
- the CLI spin-cycle regression remains `unfair-schedule witness`;
- the test-and-set spinlock, Peterson, Dekker, Bakery, and failed-CAS gallery
  witnesses all retain their explicit legacy weak label;
- finished, permanently blocked, and all-participant fairness probes remain
  fair.

The new mutex-blink fixture is the only intentional old-rule-to-new-rule
reclassification: ADR 0018 would have called that schedule fair because T1 is
disabled at some cycle states; ADR 0026 calls it strongly unfair. It is a new
fixture, not a changed committed golden.

Oracle summary text gains strongly-unfair counter fields and changes its
program/schedule totals only where a new fixed discriminator was added. Meter
output and existing CLI labels remain byte-identical.

## Verification and measured result

The final uninstrumented Release tree and the Debug tree configured with
`DPOR_ENABLE_RESTORE_ASSERTS=ON` each passed all 27 suites. The acceptance runs
took 31.19 seconds and 379.82 seconds respectively. The widened deterministic
gates reported:

- two-thread SC: 22,419 programs, 61,091 naive schedules, 34,111 DPOR
  schedules;
- three-thread SC: 65,546 programs, 896,259 naive schedules, 347,251 DPOR
  schedules;
- TSO: 10,776 programs, 136,101 naive schedules, 28,030 DPOR schedules, zero
  capped skips;
- PSO: 5,657 programs, 85,820 naive schedules, 15,107 DPOR schedules, zero
  capped skips;
- fuzz: 3,000 generated, 2,983 compared, 17 capped, 766,447 naive schedules,
  and 26,830 DPOR schedules. The seeded random corpus contained no strong
  witness in either explorer, while the separate fixed discriminator observed
  one in both.

Each two-thread and buffered oracle observed fair, strongly-unfair, and
weakly-unfair class existence under both naive and DPOR exploration. The
three-thread gate enforces the same non-vacuity with fixed fixtures.

The complete optimality-meter stdout was compared directly against a pristine
`HEAD 8db514e` Release build and was byte-identical, including SC 1.067, TSO
1.152, and PSO 1.154. The legacy test-and-set spinlock report was also
byte-identical and retained `unfair-schedule witness`; a fresh never-enabled
semaphore witness was byte-identical and retained `fair divergence`.

Two fresh semaphore-based CLI probes, distinct from the mutex unit fixture,
were checked with both explorers and replayed explicitly. A post/wait loop made
the peer's wait endpoint blink and produced
`strongly-unfair-schedule witness`; a peer waiting on a semaphore that is never
posted remained `fair divergence`.

Release wall time was measured per ADR 0023 with three serial runs of each tree
interleaved on the same host. Baseline times were 29.14, 22.84, and 46.39
seconds; campaign times were 22.81, 47.89, and 50.19 seconds. The spread shows
substantial external load, so the acceptance comparison uses each tree's best:
22.84 seconds baseline versus 22.81 seconds campaign, a 0.13% improvement and
no regression beyond noise. All six timing runs passed 27/27.

## Consequences

- Weakly fair but strongly unfair witness schedules are no longer described as
  fair divergence.
- Existing weak-unfair output is preserved by construction rather than by an
  assumed endpoint-equivalence argument.
- A genuinely never-enabled non-participant remains distinguishable from a
  blinking one.
- Replay, naive exploration, and DPOR share one deterministic classifier and
  one exact report value.
- The report remains narrower than program-level liveness and narrower than
  per-action fairness within a participating thread.

## Invariants protected

- **Replay identity:** fairness is recomputed from the same exact witness and
  enabledness semantics, including TSO/PSO flush endpoints.
- **Backward compatibility:** the weak predicate and both legacy strings are
  unchanged; strongest-label selection only subdivides formerly fair cases.
- **Determinism:** ordered exact endpoints and replayed cycle states are the
  complete input to classification.
- **Sound accounting:** every detected cycle belongs to exactly one of the
  three counters, and complete naive/DPOR gates compare each class's existence.
- **Cost isolation:** programs without a detected lasso pay no new exploration
  or classification cost.
