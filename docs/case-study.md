# Case study: verifying a DPOR model checker against itself

This document is the story of how this checker was built and — more
importantly — how it was kept sound. The short version: **every pruning
algorithm in this project is checked against an exhaustive oracle by three
differential gates, and those gates caught two real soundness bugs that had
survived careful human review.** Everything else is architecture in service
of that loop.

## The core bet: keep the slow oracle forever

Dynamic Partial-Order Reduction is easy to get subtly wrong. The literature
is full of variants, and the failure mode is silent: a checker that prunes
one schedule class too many simply *misses bugs*, and nothing in normal
operation tells you. So the first architectural decision (ADR 0001, 0003)
was that the naive exhaustive enumerator is not scaffolding to be deleted —
it is a permanent reference semantics, and DPOR must continuously prove
verdict-equivalence against it:

- same race / deadlock / modeled-error existence verdict on every program,
- never more explored schedules than the oracle,
- every DPOR report must replay, step for step, to an identical report.

Three gates enforce this on every commit (CI, Linux + macOS):

| Gate | Coverage | Shape |
|---|---|---|
| 2-thread sweep | ~21k programs, 17-action alphabet | exhaustive per length pair |
| 3-thread sweep | 65,542 programs, 15-action alphabet | even stride over the 10^6+ space |
| Differential fuzz | 3,000 programs/run, 2–5 threads | seeded, reproducible, includes malformed programs |

A fuzz failure prints the seed and the program in the CLI's text format, so
any divergence becomes a runnable `.dpor` file.

## Bug one: the repair point that was too clever

The first DPOR implementation (ADR 0003) was deliberately conservative:
when a dependent transition was *disabled* at its potential backtrack
point, it added every enabled thread at that point — the classic fallback
that preserves deadlock discovery. The upgrade to happens-before-aware,
last-point backtracking (ADR 0006) replaced this with a nonstandard
narrowing: apply the fallback only at the *earliest* dependent disabled
prefix. The argument — maximal reordering freedom plus inductive
re-analysis — was reviewed, rated plausible, and accepted on the strength
of ~217,000 gate programs passing.

It was wrong anyway. When dynamic thread creation landed (ADR 0010), the
gates went red on spawn/join/condvar deadlock patterns: programs whose
deadlock was only reachable by branching at a *later* disabled prefix that
the "earliest" rule skipped. The fix restored repairs at every dependent
disabled prefix, minus one provably useless point (at or before the blocked
thread's own `spawn`, where it cannot run). ADR 0011 later made this
efficient again by computing each blocked transition's *enabler chain* —
who could actually unblock it — instead of adding everyone.

## Bug two: the sleep set that ate a backtrack

The same spawn-era gate failures exposed a second, nastier defect that had
been latent since sleep sets were introduced: inserting a thread into a
node's backtrack set did not remove it from that node's *sleep set*. A
sleep-set entry means "exploring this thread here is redundant with an
already-explored execution" — but a dynamic backtrack insertion is a proof
of the opposite. With both flags set, the sleep check won and the demanded
re-exploration was silently skipped.

This is precisely the class of bug that makes DPOR dangerous: locally both
rules are correct, their interaction loses schedule classes, and no test of
either mechanism alone can see it. It survived the initial sleep-set
review and ~200k gate programs; it fell to fuzz programs shaped by a new
semantic feature. The fix is one line — backtrack insertion evicts the
sleep entry — plus a regression argument in ADR 0010.

## The discipline: quantitative bars and honest stops

Two attempted improvements did not ship, and their ADRs are part of the
artifact on purpose:

- **ADR 0009** — an undo-log rewrite to eliminate per-branch state copies
  was deferred after measurement showed the Release-mode gates run in
  ~3.5s: the real cost had been an unoptimized default build, fixed with
  one CMake default instead of hundreds of lines in the exploration core.
- **ADR 0012** — a Source-DPOR attempt produced a rename plus a ~0.05%
  improvement, without the paper's weak-initials/wakeup-tree machinery.
  It failed its pre-declared success bar and was reverted; the ADR records
  the measured numbers and exactly what a real upgrade requires, per
  blocking kind.

Every shipped loop had the inverse structure: a written work order with a
quantitative acceptance bar, implementation, then independent adversarial
review with fresh probe programs and fuzz seeds beyond the ones used
during development.

## Where it ended up

- Six synchronization domains: plain memory, acquire/release atomics
  (SC-per-location, with the store-*replaces*-clock subtlety trap-tested in
  both directions), blocking mutexes, Mesa condition variables with
  two-phase waits and real lost wakeups, `join`, and `spawn` with dynamic
  aliveness.
- Backtrack-set DPOR with sleep sets, HB-aware last-point backtracking,
  enabler-chain disabled repairs, and a thread-identity-aware independence
  refinement — exploring ~10% of the oracle's schedules on the fuzz gate
  (108,868 vs 1,047,152) and exactly the trace-class count on
  independence-heavy programs.
- Reports carry replay-validated, 1-minimal reproducing schedules, and the
  CLI round-trips them: `dpor check` emits a `schedule:` block that
  `dpor replay` turns back into the identical report.
- Twelve ADRs, 13 deterministic test suites, three differential gates, CI
  on two platforms.

## The second campaign: data, real algorithms, and an instrument

A follow-on campaign took the checker from fixed-control-flow toys to real
algorithms, in three moves:

1. **Data in the IR** — thread-local registers, valued memory, branches,
   CAS (with the acquire-only-failure clock edge trap-tested in both
   directions), first-class assertion failures, and per-thread step bounds
   so spin loops verify as `clean up to bound`, never a false plain clean.
2. **The classic gallery** — Peterson, Dekker, a bounded bakery, and a
   Treiber-style CAS-retry push, each paired with a deliberately broken
   variant. The checker found every planted bug and produced zero wrong
   verdicts. Building the gallery also exposed a reporting gap (a verdict
   printed after exploration hit the schedule cap looked verified when it
   was not) — fixed as `exploration_capped`.
3. **The optimality meter** — a fourth gate that brute-force counts
   Mazurkiewicz classes on 7,162 small programs and compares DPOR's
   exploration against the theoretical minimum. Result: **redundancy ratio
   1.067, with 93.4% of programs already explored optimally** — and, for
   free, a continuous re-proof of the theory: all 26,922 schedules agreed
   with their class representative's verdict.

The meter also settled an open question by measurement: wakeup trees (the
deferred optimal-DPOR upgrade) would buy at most 6.7% at metered scales,
in exactly the code region where both historical bugs lived. The attempt
stays closed — now by instrument, not estimate.

## The third and fourth campaigns: weak memory, then proof of divergence

Two further campaigns took the checker beyond sequential consistency and
beyond bounded verdicts:

- **TSO** — per-thread store buffers with nondeterministically scheduled
  flush transitions, store-to-load forwarding, draining synchronization
  actions, and a `fence` keyword. The semantics are held up by litmus
  differentials (store-buffering's relaxed outcome reachable under TSO,
  unreachable under SC, unreachable again with fences) and a TSO oracle
  gate. The gallery demo is three command lines: Peterson under SC shows
  no assertion; under TSO, `also_found: assertion` — store buffering
  breaks mutual exclusion; fenced, it holds again.
- **Lasso detection** — an execution that revisits an exact canonical
  state (buffers included, clocks excluded, field audit in ADR 0016) is a
  *theorem* that some interleaving diverges, reported with a replayable
  stem+cycle witness validated by state equality. Gallery verdicts
  upgraded from `clean up to bound` (a budget shrug) to `nontermination`
  (a proof), while a growing-state litmus keeps the bound backstop
  honest. Witnesses ship unminimized because a sound lasso-preserving
  minimizer was not available — declined, documented, not faked.

## The fifth campaign: PSO, fairness, and instrument completeness

- **PSO** — per-address store buffers, whose crux is that DPOR's
  same-thread ordering rule would silently collapse the very reorderings
  PSO exists to expose; the fix forces every co-enabled per-address flush
  into the persistent choice set. The litmus gradient is textbook:
  message-passing breaks only under PSO and is repaired by a fence. A new
  gate class arrived with it: **cross-model monotonicity** (SC executions
  embed in TSO, TSO in PSO, so bug existence must be monotone — 13,590
  inclusion checks, zero violations).
- **Fairness classification** — lasso witnesses now state their
  scheduling assumptions: a cycle ignored by a runnable peer is an
  *unfair-schedule witness* (Peterson's spins), while a cycle every
  outsider is disabled against is a *fair divergence* (a mutual back-off
  retry loop classifies this way). The field describes the witness, not
  the system — recorded prominently rather than overclaimed.
- **Instrument completeness** — the optimality meter now covers all three
  memory models with the per-model same-thread flush ordering encoded
  exactly (11 TSO classes vs 12 PSO classes on the discriminator
  fixture). Baselines: SC 1.067, TSO 1.096, PSO 1.098 — the pruner stays
  within ~10% of the theoretical minimum across the family, and the gap
  is recorded as quantified headroom, not tuned away.

## The sixth campaign: private enqueues and a moving denominator

- **Buffered enqueue independence** — under TSO/PSO, a source `Write` is
  not yet a shared write: it advances its owner's PC and clock and appends
  to that owner's private buffer. Against another thread's transition the
  two execution orders leave identical buffers, shared state, race metadata,
  and enabled sets. The checker now uses that commuting diamond after the
  existing spawn/join safeguards; the later `Flush` keeps every same-address
  visibility dependency and remains the race-recording write.
- **The denominator moved too** — widening the checker's independence
  relation also widened the meter's Mazurkiewicz relation, collapsing each
  buffered corpus from 1,187 classes to 984. The old headline had therefore
  understated the pruner's redundancy:

  | Model | ADR 0019 baseline | Honest widened baseline | Shipped ratio | Shipped / old classes |
  |---|---:|---:|---:|---:|
  | TSO | 1.096 | 1.322 | 1.152 | 0.955 |
  | PSO | 1.098 | 1.324 | 1.154 | 0.957 |

  The below-one historical ratios prove that 1,187 was no longer a class
  lower bound. The predeclared 1.086/1.088 bar had aimed for a 0.010 gain in
  the old units; on a like-for-like denominator the change gained 0.170.
- **The first release disagreement** — the implementing agent recommended
  revert under the stop clause because 1.152/1.154 missed the literal bar.
  The orchestrator shipped: the commuting proof survived adversarial review,
  all gates stayed green, and the bar's denominator had been invalidated by
  the diagnosis itself. ADR 0020 preserves both positions. The disagreement
  is part of the instrument: implementation and release authority remained
  separate, and the losing argument stayed in the record instead of being
  edited away.
- **The shipped effect and remaining headroom** — `tso_oracle` fell from
  51,878 to 33,618 DPOR schedules and `pso_oracle` from 26,455 to 17,188,
  about 35% each, with verdicts unchanged and all 23 suites green. The honest
  meter leaves 150 TSO and 152 PSO excess representatives. Removing those
  requires weak initials or wakeup trees — ADR 0012 territory, not another
  local flush clause.

## The seventh campaign: reader-writer locks without invented order

- **The synchronization primitive was designed gate-first** — each rwlock
  carries a writer-release clock plus an accumulator joining every reader
  release. `RLock` consumes only the writer clock; `RUnlock` contributes to
  the accumulator; `WLock` consumes both and resets the reader epoch; and
  `WUnlock` publishes the writer clock. There is deliberately no
  reader-release-to-reader-acquire edge: read-lock holders may overlap, so
  their unprotected writes elsewhere must remain unordered and race. Four
  verdict-flipping probes pin writer-to-reader, writer-to-writer, every
  reader-to-writer edge, and the absence of reader-to-reader ordering. An
  independent writer-free CLI adversary still found that last race in three
  DPOR schedules.
- **Reader commutation has two proof boundaries** — co-enabled
  `RLock(m)`/`RLock(m)` actions form a direct commuting diamond: both orders
  leave the same holder set, clocks, race metadata, and enabled set, including
  identical `WLock(m)` disabledness after either first reader. A narrower
  checker-local rule commutes all reader-mode actions only when the whole
  program contains neither `WLock(m)` nor `WUnlock(m)`. That static exclusion
  removes the middle-writer witness; writer-bearing programs keep the other
  same-lock pairs dependent. The three-reader discriminator has
  `9! / (3!^3) = 1,680` naive leaves and exactly one DPOR leaf.
- **The gates widened rather than weakened** — the two-thread oracle now
  checks 21,856 programs over 21 actions; the three-thread sweep checks 65,543
  programs over 19. Fixed-seed fuzz generated each of the four rwlock actions
  more than 600 times, and cross-model monotonicity completed 17,040 inclusion
  checks with zero skips. The optimality corpus was deliberately untouched:
  its output remained byte-identical at SC 1.067, TSO 1.152, and PSO 1.154.
  All 24 suites passed after the extension.
- **The gallery closes the loop** — `readers_writers.dpor` is clean at seven
  naive schedules and two DPOR representatives. Its broken pair omits the
  reader lock, making both the payload race and the overlap assertion
  reachable: 56 naive schedules, six under DPOR.

## Takeaway

The takeaway this project argues for: **in this domain, review confidence
is not evidence.** Both bugs above were reviewed and rated sound. The
mechanism that actually protected soundness was structural — an oracle
that never went away, gates wide enough that new semantics kept re-testing
old pruning, and finally an instrument that measures the pruner against
the theoretical optimum on every CI run. The sixth campaign adds one
constraint on that instrument: when a semantic refinement changes its class
relation, the denominator must be re-proved rather than treated as fixed.
