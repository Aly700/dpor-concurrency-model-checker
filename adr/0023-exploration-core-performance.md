# ADR 0023: Path-local cycle history and exploration-core cost control

## Status

Accepted.

## Context

ADR 0009 deferred an in-place `ExecutionState`/undo-log rewrite while the two
largest Release gates were about 3.5 seconds each. Its revisit clause asked for
new measurement once programs, traces, or thread counts grew, or once profiling
showed state copying dominating a real workload.

That clause fired. The gate set is now 25 suites and includes a 65,544-program
three-thread sweep, a 22,126-program two-thread sweep, 3,000 deterministic fuzz
programs, TSO/PSO oracles, the optimality meter, and a large classic-algorithm
gallery. On the campaign machine, the single pre-change uninstrumented Release
`ctest` took 52.56 seconds. Its three heaviest suites were gallery 20.86s, fuzz
13.07s, and the three-thread oracle 7.41s.

Sampling profilers were unavailable under the host sandbox, so diagnosis used
a separate default-off profiled library with deterministic logical counters.
No core decision uses wall time, randomness, threads, or unordered iteration;
timing remains in the harness. Counters are exact for the instrumented process
and named path, but are lower bounds for whole-suite work: gallery CLI child
processes are uninstrumented, report-copy counters omit some downstream value
copies, and direct enabledness probes outside the collection helper are not
counted.

The pre-change counters identified these multipliers:

| Workload | Branch state copies | History copies | History entries copied | History key bytes copied | Fingerprint bytes built |
|---|---:|---:|---:|---:|---:|
| 3-thread oracle | 2,701,592 | 1,751,743 | 6,325,692 | 2,663,072,423 | 1,125,948,670 |
| fuzz | 3,664,993 | 3,548,063 | 40,084,177 | 24,164,412,672 | 2,147,626,906 |
| gallery process | 3,895,313 | 3,869,653 | 72,534,353 | 28,255,229,452 | 1,582,692,046 |

Full state copying was substantial: the three-thread sweep alone copied at
least 71,841,546 dynamic-container elements, 9,882,878 map nodes, 12,560,058
clock components, and 7,105,621 accumulated schedule steps. It was not the
largest measured byte multiplier, however. Deep-copying every existing exact
fingerprint key at every DFS child moved 2.66GB in that sweep and more than
24GB in each of fuzz and gallery. Exact fingerprint construction was the next
large acyclic cost. The three-thread process also performed 3,587,425 full
enabled-set collections, 1,704,592 transition-map builds, 1,321,728 report
schedule copies, and 340,647 replay calls.

Some workload cost is deliberate oracle work rather than reducible DPOR core
overhead. Fuzz explored 1,002,798 naive leaves versus 87,775 DPOR leaves, and
the three-thread sweep explored 845,471 naive versus 362,789 DPOR leaves. The
campaign therefore targets repeated representation work while retaining those
reference explorations unchanged.

## Decision

Do not introduce a complete in-place `ExecutionState` undo log. The measured
leading multiplier has a smaller, auditable fix, while branch state copies
remain useful isolation around the invariant-dense interpreter.

1. Each exploration owns one path-local `StateHistory`. A novel exact
   behavioral fingerprint is inserted before descent, and an RAII guard erases
   that exact map iterator on return. The previous implementation copied the
   complete map at each child. Root state/history are materialized once and the
   root state is moved into DFS.
2. Debug builds can enable `DPOR_ENABLE_RESTORE_ASSERTS`. Every depth-one
   boundary and a deterministic FNV-derived 1/1024 sample of deeper schedules
   retains reference copies of the complete parent `ExecutionState` and
   `StateHistory`, explicitly restores the insertion, and asserts exact value
   equality. No sampling input depends on time, random state, allocation
   addresses, or OS scheduling.
3. A program without a label-normalized self/backward `BranchNonzero` skips
   fingerprint/history construction entirely. Without such a branch, ordinary
   source actions advance a normalized pc; the first `Wait` phase changes
   fingerprinted wait/ownership state and cannot return without a pc-advancing
   signal; a non-last `BarrierWait` changes the fingerprinted arrival set and
   cannot return without another participant's pc-advancing arrival releasing
   the generation; and TSO/PSO flushes strictly drain finite pending buffers.
   Therefore no complete behavioral state can repeat. Missing labels remain
   forward terminal errors. A future same-pc repeatable or pc-decreasing action
   must update this classifier and proof.
4. DPOR collects the sorted enabled vector once per node and builds its
   effective-transition map from that same vector. Empty parent sleep sets
   bypass child transition-map construction because inheritance must return the
   empty set. Ordering and map type remain unchanged.
5. Retain the default-off deterministic counter harness and its focused test.
Normal Release targets contain no metric counter branches or metric output.

### Classifier extension for cyclic barriers

ADR 0024 adds the first same-pc wait-like action since this proof was accepted.
It extends, rather than invalidates, the well-foundedness argument. A valid
non-last barrier arrival strictly grows the fingerprinted sorted arrival set
and disables that participant, so the same thread cannot execute the action
again in that generation. The set can shrink back to empty only when another
participant performs the last arrival; that source step advances every parked
participant beyond its barrier pc. With no self/backward branch, none of those
participants can return to that action index. `parties == 1` advances
immediately, while zero or mismatched party counts advance into a terminal
modeled error. A buffered-model arrival first requires finite pending buffers
to drain through the already proved flush measure.

The barrier's absolute generation ordinal and accumulated arrival clock are
excluded from behavioral fingerprints because neither is program-observable.
Including the ordinal would prevent a balanced cyclic-barrier loop from ever
closing a real lasso. Every nonempty barrier instead contributes its name,
configured party count, and sorted current-generation arrival set. Thus an
undersubscribed barrier cannot be mistaken for its pre-arrival state, while an
actual released generation can return to an equal behavioral state only when
backward control flow makes cycle detection active. Focused classifier tests
cover an acyclic completed generation, an acyclic undersubscribed generation,
a blocked backward loop, and a balanced cyclic-barrier lasso with exact history
restore.

### Classifier extension for TryLock

ADR 0025 adds `TryLock`, but it requires no new well-founded measure. Every
attempt is a source transition that advances the normalized pc exactly once,
whether it acquires the mutex and writes `1` or observes an owner and writes
`0`. Its only behavioral outcomes are already represented by the existing
register array and mutex-owner map. Thus an acyclic program containing
`TryLock` remains eligible for history/fingerprint elision under the original
ordinary-source-step proof.

A retry loop necessarily contains the self/backward `BranchNonzero` that
returns control to the attempt, so the classifier enables exact cycle history
before a failed-try lasso can close. The focused metrics gate pins both edges:
an acyclic `TryLock`/`Unlock` program builds no fingerprints, while a failed
`TryLock` retry under a retained owner detects a cycle and balances every
history insertion with its restore.

After these changes, the profiled three-thread sweep retained all 2,701,592
branch state copies but reduced history copies and fingerprint bytes to zero.
Enabled collections fell from 3,587,425 to 2,810,854 (776,571 fewer, 21.6%),
and transition maps from 1,704,592 to 1,465,612 (238,980 fewer, 14.0%). This is
evidence that the speedup comes from the named removed work rather than a
smaller explored state space: its program and schedule totals are unchanged.

## Verification and measured result

The final Release timing and complete per-suite before/after table are recorded
below after the acceptance run.

The acceptance measurement compared a pristine `HEAD 5707a8c` Release tree
(exported with `git archive` and built with identical flags) against the
campaign tree on the same machine. Each tree ran the full 25-suite `ctest`
three times, interleaved. Other worker sessions were active on the host during
the window and one baseline run measured 94.86 seconds against a 50.85-second
best, so single-run wall times are not trustworthy; the recorded result is the
per-tree best of three, which rejects intermittent external load. Both trees
passed 25/25 in every run.

| Total Release `ctest` | Baseline 5707a8c | Campaign | Improvement |
|---|---:|---:|---:|
| Best of three (wall) | 50.85s | 26.92s | 47.1% |
| Per-suite best-of-three sum | 47.46s | 26.07s | 45.1% |
| First-run user CPU | 45.72s | 23.01s | 49.7% |

This clears the predeclared bar of at least 30% Release wall-time improvement.
Per-suite best-of-three times for every suite above the 0.15-second noise
floor:

| Suite | Baseline | Campaign | Improvement |
|---|---:|---:|---:|
| classic_gallery_tests | 20.29s | 13.58s | 33.1% |
| dpor_fuzz_differential | 13.14s | 4.37s | 66.7% |
| dpor_oracle_3threads | 7.07s | 4.25s | 39.9% |
| dpor_optimality | 3.14s | 1.67s | 46.8% |
| tso_oracle | 1.52s | 0.72s | 52.6% |
| pso_oracle | 0.94s | 0.41s | 56.4% |
| dpor_oracle | 0.70s | 0.32s | 54.3% |

The gallery suite improves least because much of its time is spent in
uninstrumented CLI child processes whose parse/report work this campaign does
not touch. `cli_tests` (0.37s vs 0.58s at best) sits at the subprocess noise
floor and is dominated by process startup rather than exploration.

Byte-identity was verified directly, not inferred from passing tests: the
baseline and campaign binaries for `dpor_oracle`, `dpor_oracle_3threads`
(845,471 naive / 362,789 DPOR schedules), `tso_oracle`, `pso_oracle`, and
`dpor_optimality` produce byte-identical stdout, including the pinned meter
lines SC 1.067, TSO 1.152, PSO 1.154.

The Debug tree with `DPOR_ENABLE_RESTORE_ASSERTS=ON` passed all 25 suites in
258.31 seconds, exercising the depth-one and sampled deep restore-exactness
assertions across the full gate set.

Behavior is required to remain byte-identical: oracle summary lines, verdicts,
witness artifacts, schedule counts, and optimality meter lines. The pinned
meter values are SC 1.067, TSO 1.152, and PSO 1.154.

A pre-existing Debug-only test defect was exposed by the assertion build: two
fixtures expected two DPOR leaves although both pristine `HEAD 5707a8c` and
the campaign tree explore three (with 30 naive leaves). Release had compiled
the test's plain `assert` away. The fixture expectations are corrected to three
and the comparison is made non-elidable with a diagnostic. Compiling that
corrected test against pristine HEAD's model library reproduced the unchanged
full oracle summary (60,791 naive; 34,481 DPOR), so this is a test repair, not
an exploration change.

## Consequences

- Exact lasso detection retains its canonical, collision-free state equality
  on every program capable of backward control flow, including cyclic-barrier
  programs whose observable state repeats across absolute generations.
- The dominant history-copy and acyclic-fingerprint traffic is removed without
  applying/undoing interpreter mutations or weakening branch isolation.
- Exploration choice order, ordered containers, reports, and public API remain
  unchanged.
- Debug restore checking is intentionally expensive and opt-in. It validates
  history undo and the complete parent-state boundary; branch `ExecutionState`
  itself is still copied rather than mutated in place.
- Full state copying is now the main remaining general-purpose copy cost. A
  future revisit should profile longer cyclic traces or heavier buffered state
  before considering copy-on-write or interpreter-wide undo logs.

## Invariants protected

- **Soundness:** history is scoped to one DFS path and exact cycle equality is
  unchanged wherever a cycle is possible; same-pc barrier arrivals satisfy the
  extended acyclic-classifier proof because arrivals are fingerprinted and
  release advances every parked pc.
- **Restore exactness:** every inserted key is erased once, with deterministic
  sampled comparison against full reference values in Debug builds.
- **Replay and behavior:** schedules, order, verdicts, witness bytes, oracle
  totals, and meter output are acceptance-locked.
- **Determinism:** profiling, sampling, enabled reuse, and restore use only
  deterministic program/schedule data and ordered containers.
