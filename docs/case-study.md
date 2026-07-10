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

## Takeaway

The takeaway this project argues for: **in this domain, review confidence
is not evidence.** Both bugs above were reviewed and rated sound. The
mechanism that actually protected soundness was structural — an oracle
that never went away, gates wide enough that new semantics kept re-testing
old pruning, and finally an instrument that measures the pruner against
the theoretical optimum on every CI run.
