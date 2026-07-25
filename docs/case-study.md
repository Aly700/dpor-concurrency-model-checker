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
  Campaign fourteen later found the missing precondition in that historical
  proof: the diamond remains valid only when no `Upgrade(m)` can become a
  newly enabled middle transition after the first acquisition. The original
  four-action language satisfied that condition; the extended language makes
  it an explicit per-name static guard.
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

## The eighth campaign: dining philosophers, then strong semaphores

- **The gallery gained its canonical deadlock** — three philosophers taking
  left fork then right fork reach the circular wait in which every thread owns
  one mutex and blocks on the next. The minimized witness names all three fork
  owners and replays identically under the naive explorer, DPOR, and the CLI.
  The paired fix imposes a total fork order: the last philosopher reverses its
  local acquisition order, the cycle disappears, and the model verifies clean.
- **Semaphore happens-before is strong by declaration, not accident** — permits
  are anonymous, start at zero, and are seeded only by explicit `SemPost`
  actions. Every post release-joins its post-tick clock into a lifetime
  accumulator; every successful `SemWait` decrements the count and
  acquire-joins the whole accumulator without clearing it. A waiter can
  therefore become ordered after posts whose permit it did not consume. Exact
  permit-to-wait matching would add replayable nondeterminism, so the checker
  verifies against this declared strong model rather than pretending to model
  exact permit passing.
- **The edge was tested by breaking it** — poster-publishes/waiter-reads is
  race-free with the accumulator join; removing that join flips the probe to a
  race, and restoring it restores the verdict. `SemPost` never acquires the
  existing accumulator, so two posters' otherwise unrelated plain writes stay
  unordered and still race. A worker was killed while the deliberate mutation
  was live, so the experiment was restarted by inspecting and restoring the
  known-good join before mutating again — a small infrastructure-resilience
  rule for destructive probes.
- **Commutation shipped at one proved boundary** — co-enabled
  `SemPost(s)`/`SemPost(s)` actions commute because count addition and clock
  join commute and either first post enables the same waiters. Every same-name
  pair involving `SemWait` stays dependent; a zero-permit wait deliberately
  uses the all-enabled repair so alternate-poster middle witnesses survive.
  The discriminator has three naive leaves and two DPOR representatives. The
  widened gates covered 22,126 two-thread and 65,544 three-thread oracle
  programs plus 3,000 fuzz programs; meter output stayed byte-identical at SC
  1.067, TSO 1.152, and PSO 1.154, and all 25 suites passed.

## The ninth campaign: a deferral that reopened itself

- **ADR 0009's revisit clause did what a deferral clause is supposed to do** —
  the original undo-log proposal had been declined while the Release gates ran
  in roughly 3.5 seconds, with explicit instructions to reopen the decision if
  real workloads grew or profiling found copy cost dominant. Eight campaigns
  later the suite's honest pristine best was 50.85 seconds: 25 suites now
  included 65,544 three-thread programs, 22,126 two-thread programs, 3,000 fuzz
  programs, buffered-model oracles, the gallery, and the optimality meter. The
  old decision was not treated as precedent to defend; its own instrument
  fired, so the project measured again.
- **Diagnosis overturned the suspected mechanism** — the sandbox would not
  permit a sampling profiler, so a default-off
  `DPOR_BUILD_EXPLORATION_PROFILE` build counted deterministic logical work
  instead. Per-branch `ExecutionState` copies were large, but they were not the
  leading multiplier. Every DFS child also copied the complete exact
  `StateHistory`; one fuzz run copied 40,084,177 history entries and more than
  24GB of fingerprint-key bytes. Gallery crossed 28GB. That finding changed
  the implementation target: optimize the measured representation cost, not
  the cost ADR 0009 had guessed at deferral time.
- **The shipped fix is deliberately smaller than an interpreter-wide undo
  log** — one history map now belongs to the active path, with an RAII guard
  erasing the exact child insertion on every return. Programs with no
  label-normalized self/backward branch skip cycle history entirely: source
  steps monotonically advance normalized PCs; a first-phase `Wait` cannot
  restore its prior wait/ownership state without a signaling source step; and
  TSO/PSO-only flush progress strictly drains finite buffers. The same sorted
  enabled vector is also reused to build the DPOR transition map, and empty
  sleep sets bypass vacuous inheritance work. Branch state copies remain in
  place, preserving the interpreter's easy-to-audit isolation instead of
  concentrating lossy mutation restoration in its most invariant-dense code.
- **Verification checked bytes and restoration, not just verdict headlines** —
  pristine and campaign binaries produced byte-identical oracle and meter
  stdout, including 845,471 naive / 362,789 DPOR three-thread schedules and SC
  1.067 / TSO 1.152 / PSO 1.154. A Debug build retained full reference copies
  at every depth-one boundary and a deterministic sample of deeper nodes, then
  asserted exact parent-state and history equality after undo; all 25 suites
  passed in 258.31 seconds. That build also exposed a pre-existing test defect
  that Release's `NDEBUG` had hidden: two plain-`assert` fixtures expected two
  DPOR leaves although the checker had long produced three from 30 naive
  leaves. The corrected non-elidable check passed when compiled against
  pristine `5707a8c`, establishing that the defect predated the campaign and
  making the case for keeping an assertion-enabled gate.
- **The performance bar survived a noisy machine instead of being negotiated
  away** — the first single-run comparison read **27.8%**, below the declared
  30% bar. Concurrent sibling workers were contaminating wall time in both
  directions; one pristine run took 94.86 seconds while its best took 50.85.
  Rather than ship on that outlier or lower the bar, the acceptance record used
  symmetric interleaved best-of-three runs: 50.85 seconds pristine versus
  26.92 seconds campaign, a **47.1%** wall-time improvement, corroborated by a
  49.7% reduction in user CPU. The win became acceptable only after the noise
  was handled with an explicit honest estimator; the initially failing 27.8%
  reading remains in the story.

## The tenth campaign: collective barriers and the last arriver

- **Barrier happens-before is collective, not pairwise** — for generation `g`,
  each arrival joins its post-tick clock into `R_g`; release then joins `R_g` —
  the join of **all** generation-arrival clocks — into every participant before
  any can continue. Completion clears both the arrival set and `R_g` before the
  barrier name is reused, so a later generation cannot inherit a false edge
  merely by naming the same barrier. Real cross-generation order survives in
  the clocks of participants that actually carry it through program order and
  arrive again.
- **The first new same-pc wait-like action since ADR 0023 forced an explicit
  proof amendment** — a non-last `BarrierWait` is a completed transition whose
  participant remains at the same source endpoint. ADR 0023's
  fingerprint-elision classifier could not simply assume its old
  well-foundedness argument still applied: the current generation's sorted
  arrival set had to enter the behavioral fingerprint. In an acyclic program
  each non-last arrival strictly grows that set and disables its thread; only
  another thread's source arrival can empty it while advancing every parked pc,
  after which no participant can return to the old endpoint without backward
  control flow. That extension, rather than analogy, keeps elision legal.
- **Early-arrival independence is state-dependent and deliberately narrow** —
  with `k` arrivals and party count `p`, two co-enabled same-generation
  arrivals commute only when `k + 2 < p`. Equality means the second transition
  releases the generation and therefore stays dependent. Four safeguards make
  that local diamond usable by the full reducer: persistent closure retains all
  co-enabled same-generation arrival siblings; sleep inheritance uses the
  parent's barrier snapshot and requires the exact occurrence to remain
  enabled; incomplete-barrier repairs keep the conservative all-enabled
  fallback while a last arrival depends on the later actions it releases; and
  generation stamps distinguish cyclic occurrences that share the same public
  `(thread, action_index)`.
- **The pruning claim has an exact three-party witness** — three one-action
  threads produce all `3! = 6` arrival orders under naive exploration. For each
  possible last arriver, the other two arrive early at `k == 0` and commute, so
  their two orders collapse to one representative. The completion transition
  remains dependent, leaving exactly three DPOR schedules: one class per last
  arriver, with no attempt to collapse the collective release itself.
- **The gates widened while the meter did not move** — the two-thread oracle
  checked 22,269 programs over 24 actions (79,060 naive / 35,882 DPOR), the
  three-thread sweep checked 65,544 over 22 (833,863 / 336,551), TSO checked
  10,698 over 12 (121,409 / 28,970), and PSO checked 5,579 over 12 with zero
  capped skips (54,521 / 15,920). Fixed-seed fuzz generated 1,100 barrier waits
  across 3,000 programs, comparing 2,977 and reporting 23 capped cases;
  cross-model inclusion completed 17,170 checks over 1,717 programs, including
  all four barrier programs with zero barrier skips. Both 26-suite build
  flavors passed, and the optimality lines remained byte-identical at SC 1.067,
  TSO 1.152, and PSO 1.154.

## The eleventh campaign: try-lock, false edges, and the third holder

- **Try-lock made mutex happens-before path-sensitive** — a successful attempt
  is exactly `Lock`: it takes ownership, writes `1`, and acquire-joins the last
  release frontier. A failed attempt writes `0`, advances, and joins nothing.
  Verdict-flip fixtures pin the success edge in both thread-id directions; two
  negative fixtures pin the more dangerous failure direction. One leaves a
  stale release frontier behind a different current owner, and the other puts a
  write inside the live holder's critical section. Reads after each failed
  attempt must remain racy, proving that neither the stored frontier nor the
  holder's live clock leaks through failure. Independent adversarial review
  kept that no-edge race visible even when a separate deadlock appeared in
  `also_found`.
- **A retry loop was reclassified as scheduling behavior, not blocking** —
  mutex ownership never disables `TryLock`, and it never enters a deadlock
  blocker set. A backward branch on its `0` result instead creates an ordinary
  lasso question.
  If the spinner repeats while a nonparticipating holder's `Unlock` remains
  enabled at every cycle state, the witness is labeled `unfair-schedule`: the
  label qualifies that schedule, not the whole program, and does not invent a
  deadlock. The spin-until-acquired adversary showed both sides at once: the
  acquisition path was safety-clean because success received the real acquire
  edge, while the schedule that ignored the enabled unlock retained its unfair
  lasso witness. The gallery's correct test-and-set counter follows that
  convention; moving the counter write outside the acquired section yields a
  replayable race.
- **The only new same-mutex diamond needs a third-party owner** — two exact
  co-enabled `TryLock(m)` occurrences commute only when the node's snapshotted
  owner is a third thread distinct from both triers. Both orders then write
  `0`, advance and tick each trier once, leave ownership and all mutex clocks
  untouched, and reach the same complete state with the same enabled set. The
  checker-local relation consults the parent node's owner snapshot and requires
  the identical endpoint/action occurrence to survive sleep inheritance; an
  attempt advances its pc, so no barrier-like generation stamp is needed.
  Free-mutex attempts deliberately remain dependent: the first succeeds and
  the second fails, so reversing them changes the owner and both result
  registers rather than forming a diamond.
- **The discriminator pins three semantic classes, not just a smaller number**
  — with T0 and T1 each trying `m` once and T2 finishing while holding it,
  naive exploration has four maximal orders. T0-first and T1-first are distinct
  winner classes; after T2 locks first, the two orders of guaranteed failure
  commute. DPOR therefore explores exactly three representatives. Counts alone
  would be a weak guard, so companion fixtures keep both free-mutex winners,
  keep the owner-equals-either-trier cases dependent, reject the refinement for
  mixed same-mutex actions, and use spawn-gated fail/unlock/success plus
  asymmetric winner assertions to preserve the middle and outcome classes.
- **Every differential gate widened while the instrument stayed fixed** — the
  two-thread oracle checked 22,418 programs over 25 actions (61,087 naive /
  34,108 DPOR), the three-thread sweep checked 65,544 over 23 (896,252 /
  347,246, with 38,178 strict reductions), TSO checked 10,775 over 13 (136,097
  / 28,027), and PSO checked 5,656 over 13 (85,816 / 15,104), with zero capped
  skips in both buffered oracles. Fixed-seed fuzz generated 1,578 try-locks and
  compared 1,556 of them; 2,983 of 3,000 programs were fully compared and all
  17 capped cases were reported. Cross-model inclusion ran 17,230 checks over
  1,723 programs with zero global skips and all four dedicated try-lock programs
  compared. Both 27-suite flavors passed, the meter lines remained
  byte-identical at SC 1.067 / TSO 1.152 / PSO 1.154, and interleaved
  best-of-three Release runs moved from 23.14 seconds pristine to 22.78 seconds
  with the campaign — a 1.6% favorable difference and no wall-time regression.

## The twelfth campaign: strong fairness and the witness that blinked

- **The old two-way answer called a real scheduling violation fair** — ADR
  0018's weak classifier called a lasso unfair only when some nonparticipant
  stayed enabled at every cycle state; everything else was filed as
  `fair divergence`.
  The discriminator put that boundary under a microscope: T0 repeatedly
  unlocks and retakes a mutex while T1's pending `Lock` is enabled only in the
  released state. T1 is never continuously enabled, so the witnessed schedule
  satisfies weak fairness. But the exact cycle repeats forever, enabling that
  same endpoint once per iteration and never taking it, so the schedule violates
  strong fairness. That blinking endpoint split the old fair bucket into a real
  fair divergence and a `strongly-unfair-schedule witness`.
- **The classifier has strict precedence but no private transition
  language** — a continuously enabled nonparticipant keeps the legacy
  `unfair-schedule witness` label; otherwise an exact nonparticipant endpoint
  enabled anywhere in the cycle selects the new strong label; only a cycle with
  no such endpoint anywhere is `fair divergence`. Every weak violation is also
  a strong one logically, but the first label is the strongest diagnosis. Exact
  identity deliberately means the public replay identity —
  `(thread, action_index)` for source endpoints and TSO flushes, plus the
  canonical address for a PSO flush — rather than a fairness-only phase or
  generation key. The report therefore classifies the same deterministic
  object the CLI can replay, without creating a second transition model that
  could drift from execution.
- **Backward compatibility was treated as an invariant, not a hope** — the old
  `continuously_enabled[tid]` predicate remains the verbatim authority for the
  weak label, including its spelling. The public enum pins
  `UnfairScheduleWitness == 0` and `FairDivergence == 1`, while the new counter
  is appended to `CheckResult` so positional aggregate initialization keeps its
  meaning. The audit changed exactly zero stored golden files: the CLI spin
  cycle and five classic lasso reports retained their byte-identical weak label,
  and never-enabled witnesses stayed fair. The mutex blink was the only
  old-rule-to-new-rule reclassification, and it was introduced as a new fixture
  rather than laundering a changed golden.
- **The field remains diagnostic evidence about one schedule** — replaying a
  strongly unfair lasso proves that this exact stem and cycle postpone an
  infinitely often enabled endpoint. Scheduling the blinking peer instead may
  terminate, expose a bug, or reach a different fair or unfair cycle; a first
  witness of one class does not prove the others absent from the program. The
  compatibility scope also remains inter-thread: once a thread owns any cycle
  step, its other endpoints are excluded, so the report does not overclaim
  action fairness within a participating thread or program-level liveness.
- **The gates compare class reachability, and admit where randomness did not
  help** — every cycle-aware naive/DPOR gate now compares fair,
  strongly-unfair, and weakly-unfair witness existence independently, because
  the explorers may retain different numbers of representatives. The widened
  runs covered 22,419 two-thread programs (61,091 naive / 34,111 DPOR), 65,546
  three-thread programs (896,259 / 347,251), 10,776 under TSO (136,101 /
  28,030), and 5,657 under PSO (85,820 / 15,107), with zero buffered-oracle
  skips. The seeded 3,000-program fuzz corpus honestly found **zero** strong
  witnesses in either explorer, so a fixed uncapped blinking discriminator —
  outside those random totals — makes that comparison non-vacuous on every
  run. Both 27-suite flavors passed, meter output stayed byte-identical at SC
  1.067 / TSO 1.152 / PSO 1.154, and interleaved best-of-three Release timing
  showed no regression.

## The thirteenth campaign: symmetry measured before it was built

- **The attractive example was already solved by DPOR** — three identical
  rwlock readers look like an obvious thread-symmetry win, but their 1,680
  naive schedules already collapse to one Mazurkiewicz class and one current
  DPOR representative. Quotienting thread names cannot improve a corpus where
  the existing reduction has already removed every duplicate order.
- **Replay made the quotient expensive before search did** — a race, deadlock,
  error, assertion, or lasso names original thread coordinates. A sound
  symmetry reducer therefore needs an invertible permutation carried through
  schedules, reports, exact cycle closure, and fairness classification. The
  default-off diagnosis checked report isomorphism rather than treating a
  canonical state hash as sufficient.
- **The diagnosis justified deferral** — only 265 of 107,430 measured programs
  retained a nonidentity automorphism after whole-program and `Signal`
  safeguards, and the symmetry-only timing proxy was 0.00212813% of selected
  gate wall time. The eligible slice had a real 49.74% class reduction, but it
  was too rare and too cheap to justify the witness-translation machinery.
  ADR 0027 records the honest deferral; no shipped exploration path changed.

## The fourteenth campaign: upgrading without an unlocked instant

- **The conversion is ownership, not release plus reacquire** — `Upgrade`
  retains the caller's read hold until it is the sole reader, then atomically
  replaces that hold with write ownership. It consumes and clears the same
  accumulated reader-release frontier as `WLock`; the caller's retained hold
  was never released into that accumulator, so no self-edge is invented.
  `Downgrade` publishes the post-tick writer frontier while atomically
  retaining read ownership, leaves the reader accumulator intact, and lets the
  later `RUnlock` contribute normally. Mirrored verdict probes pin both new
  positive edges, while a plain-reader negative probe keeps reader-reader
  races visible.
- **Two upgrades expose a real wait cycle** — two readers that both retain
  their holds cannot make either thread the sole reader. A barrier-synchronized
  discriminator reports two
  `rwlock NAME upgrade_waiting_for_readers_to_drain` blockers rather than
  reentrancy errors or legacy `WLock` self-waits. Naive exploration has six
  terminal arrival orders; Upgrade-aware DPOR retains four classes, and the
  first witness is identical through both explorers, CLI check, numeric replay,
  and the stored gallery golden.
- **A local commutation proof failed on future enabledness** — two root
  `RLock(m)` steps still commute state-wise, but after only the first one its
  thread may be the sole reader and run `Upgrade(m)`. That middle action was
  not enabled at the root, so persistent closure cannot recover the opposite
  upgrader-first class if the acquisitions are blindly commuted. The public
  relation is now conservative for every same-name rwlock pair. Checker-local
  DPOR restores exact `RLock`/`RLock` independence only when that name is
  statically Upgrade-free, and restores the broader reader-mode rule only when
  it is free of every writer-mode action. An asymmetric assertion fixture pins
  the otherwise-lost upgrader-first outcome.
- **The existing state was already the right occurrence identity** — a blocked
  Upgrade does not fire; a fired Upgrade or Downgrade always advances pc.
  Successful mode is visible in the existing canonical reader set and writer
  owner, so no fingerprint, schedule, or generation field was added. Under TSO
  and PSO both conversions are explicit-drain ordered points. The strict CLI,
  all four oracle families, fixed-seed differential fuzz, cross-model
  inclusion, and the classic gallery all exercise the new actions without
  changing the SC 1.067 / TSO 1.152 / PSO 1.154 optimality instrument.
- **The final gates discriminated the design rather than blessing it** — both
  directions of each new HB probe flipped to a race when its single clock
  update was removed, while the reader-reader negative probe stayed racy after
  restoration. Release and restore-assert Debug both passed 28/28. The four
  widened oracle families and cross-model inclusion had zero skips; fixed fuzz
  generated 317 Upgrades and 363 Downgrades. In the accepted interleaved
  best-of-three timing against pristine `b432cdc`, the full Release suite
  improved from 35.74s to 33.36s despite adding the conversion suite.

## The fifteenth campaign: one wake edge, fanned out

- **Broadcast snapshots receivers; it does not mint permission** — one
  `Broadcast(cv)` atomically moves every thread currently parked on `cv` into
  its existing mutex-reacquisition phase. It never grants ownership and never
  queues a permit. An empty Broadcast followed by a Wait still produces the
  ordinary replayable lost-wakeup deadlock.
- **The HB edge fans out but never crosses between receivers** — every woken
  waiter independently joins the same broadcaster post-tick clock. Removing
  that join flips broadcaster-before-waiter probes to races in both thread-ID
  directions. Separate negative probes keep two receivers unordered after
  their critical sections and prove that an empty Broadcast stores no clock
  for a later waiter.
- **Occurrence identity names the exact fan-out** — an effective Broadcast
  carries the exact sorted parked-thread set through node matching,
  backtracking, repair, and sleep inheritance; engaged `{}` differs from an
  absent non-Broadcast component. Same-condition Wait, Signal, and Broadcast
  remain conservatively dependent. That relation already prevents
  sleep-inherited parked-set changes, but it does not make cyclic historical
  backtrack matches occurrence-exact. In a genuine stamp-removal mutation,
  two mirrored fixtures fell from 16 to 9 and from 25 to 19 DPOR
  representatives while their 3,954-schedule naive explorations and
  existential race/deadlock kinds remained intact. This is honest evidence of
  unproved class-accounting loss, not a claimed verdict flip; the exact stamp
  ships because no mechanism proves the discarded representatives equivalent.
- **Two discriminators pin the hard scheduling classes** — a multi-wake
  critical-section fixture retains both verdict-relevant reacquisition orders
  at 22 naive / 14 DPOR schedules. The forced-parking differential is clean at
  86/30 with one Broadcast, while two Signals lose the early wake and expose a
  deadlock at 43/15. The classic gallery preserves the same contrast in
  `mesa_broadcast_consumers.dpor` and
  `mesa_broadcast_consumers_broken_single_signal.dpor`, whose DPOR goldens
  report 30 and 15 schedules.
- **The corrected timing unit measures the checker core like-for-like** — the
  first stop compared 28 baseline suites with 29 campaign suites and therefore
  charged newly required coverage to the core. The accepted ruling compares
  the exact 27-suite name intersection while retaining expanded Broadcast
  corpora inside common suites. Interleaved best-of-three improved from 20.11s
  at `83e8cf9` to 19.87s, and every named exploration-core best was equal or
  lower. The four widened oracles have zero skips; fuzz generates 762 and
  compares 746 Broadcasts; cross-model inclusion's dedicated Broadcast corpus
  is 4/4/0; Release and deterministic restore-assert Debug both pass 29/29;
  and the SC 1.067 / TSO 1.152 / PSO 1.154 meter contract is unchanged.

## Takeaway

The takeaway this project argues for: **in this domain, review confidence
is not evidence.** Both bugs above were reviewed and rated sound. The
mechanism that actually protected soundness was structural — an oracle
that never went away, gates wide enough that new semantics kept re-testing
old pruning, and finally an instrument that measures the pruner against
the theoretical optimum on every CI run. The sixth campaign adds one
constraint on that instrument: when a semantic refinement changes its class
relation, the denominator must be re-proved rather than treated as fixed.
