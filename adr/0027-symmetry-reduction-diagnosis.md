# ADR 0027: Thread-symmetry reduction diagnosis — deferred

## Status

Accepted (diagnosis and deferral). No symmetry reduction is implemented. The
optional diagnosis remains default-off and cannot alter a shipped exploration
path.

## Context

Thread symmetry can identify schedules that differ only by a permutation of
behaviorally identical threads. In principle, the checker could explore one
representative of each such orbit. The tempting examples are identical readers,
identical fuzz workers, and the cyclic structure of dining philosophers.

This is not a free quotient in a replay-first model checker. Every race,
deadlock, model error, assertion, and nontermination witness names original
thread coordinates. A quotient that reports its canonical coordinates without
an invertible map would produce a schedule that the existing interpreter cannot
replay. A lasso that closes only modulo a permutation is not the exact state
closure required by ADR 0016, and translating its fairness label after the fact
would not repair that defect.

ADR 0009 requires measured pressure before rewriting invariant-dense code. ADR
0012 records that a small schedule-count improvement does not justify a
misnamed or incomplete exploration algorithm. This diagnosis applies those
precedents before designing or implementing a symmetry reducer.

## Decision

Defer thread-symmetry reduction. The exact-body opportunity in the current
gates is too small, and the witness-sound implementation cost is
disproportionate:

- 536 of 107,430 programs (0.498930%) contain an identical normalized body;
- only 265 programs (0.246672%) retain a nonidentity automorphism after
  whole-program validation and the conservative `Signal` restriction;
- 103 programs (0.095876%) are small and semantically covered by the orbit
  meter;
- all exact-body candidates account for 9,221 of 1,562,355 current DPOR schedules
  (0.590199%); and
- the conservative schedule-linear timing model attributes only 0.00212813%
  of selected-gate wall time to the symmetry-only opportunity.

The measured eligible slice does have leverage: quotienting removes 195 of 392
Mazurkiewicz classes (49.74%), and the theoretical one-per-orbit target is 197
representatives rather than the 490 schedules current DPOR explores. That is
the strongest argument for the feature, but the slice is too rare and too
cheap to affect the complete gates.

## Stage 1: measurement

### Candidate definition and normalization

A candidate program has at least two threads whose normalized bodies are
exactly equal. Normalization is intentionally narrow:

1. `Label` pseudo-actions are removed.
2. A valid branch label is rewritten to the target executable-action ordinal,
   so label spelling and harmless label placement do not create false
   differences.
3. A missing label retains a normalized spelling because its text is part of a
   public model error.
4. All action kinds, operands, register IDs, addresses, mutexes, rwlocks,
   semaphores, barriers, conditions, and values remain exact.

Candidate permutations are enumerated only within equal-body blocks. For a
candidate permutation, in-range `Spawn` and `Join` thread targets are remapped,
and every transformed source body must exactly equal its destination body.
This whole-program check separates body equality from structural
automorphism.

The interpreter's `Signal` chooses the lowest-numbered waiter. That rule is
not equivariant under arbitrary thread permutations. The diagnosis therefore
keeps only the identity permutation for any program containing `Signal`, even
when the structural check finds a nonidentity permutation. The structural and
admissible counts are reported separately.

This criterion does not rename resources. The dining-philosophers programs
therefore have no identical bodies: each philosopher names different mutexes.
A ring automorphism with simultaneous thread and mutex renaming is outside this
measurement.

### Orbit meter and exclusions

For each small admissible program, the default-off meter collects the complete
naive schedule set and replays every schedule. It computes:

- `N`: complete naive schedules;
- `D`: schedules explored by current DPOR;
- `M`: Mazurkiewicz classes, using the same effective-transition dependence
  DAG and deterministic topological form as the optimality meter; and
- `O`: those classes modulo every admitted thread permutation.

Thus `O` is the theoretical one-representative-per-orbit minimum for the
measured transition relation. `D - O` is a gross target that includes existing
DPOR redundancy. `M - O` isolates the symmetry quotient from that redundancy.

Before accepting an orbit count, the meter verifies that each mapped schedule
replays, preserves the race/deadlock outcome signature, and maps every race or
deadlock payload and witness schedule isomorphically. It also checks
`O <= M <= D <= N` and that a Mazurkiewicz class has one outcome signature.
There were no class-outcome, lower-bound, or report-isomorphism violations.

The orbit count excludes incomplete or capped exploration, cycle or bound
prefixes, model errors and assertions, and programs above 20,000 naive
schedules. It also excludes `TryLock` and `BarrierWait`: their dependence or
occurrence identity is node-sensitive, so the public transition predicate is
not sufficient to certify this offline class quotient. These are explicit
coverage limits, not zero-opportunity claims.

### Corpus prevalence

`P` is programs, `I` is exact-body candidates, `A` is programs with an
admissible nonidentity automorphism, and `E` is orbit-meter eligible programs.
`D(all)` and `D(I)` are current DPOR schedules over the full corpus and over
exact-body candidates respectively.

| Corpus | P | I (%) | A (%) | E (%) | D(all) | D(I) |
|---|---:|---:|---:|---:|---:|---:|
| two-thread SC | 22,419 | 50 (0.223) | 40 (0.178) | 26 (0.116) | 34,111 | 107 |
| three-thread SC | 65,546 | 348 (0.531) | 155 (0.236) | 29 (0.044) | 347,251 | 2,343 |
| TSO oracle | 10,776 | 26 (0.241) | 26 (0.241) | 20 (0.186) | 28,030 | 1,226 |
| PSO oracle | 5,657 | 26 (0.460) | 26 (0.460) | 20 (0.354) | 15,107 | 1,226 |
| fuzz | 3,000 | 86 (2.867) | 18 (0.600) | 8 (0.267) | 26,830 | 4,319 |
| classic gallery | 32 | 0 (0.000) | 0 (0.000) | 0 (0.000) | 1,111,026 | 0 |
| **Total** | **107,430** | **536 (0.499)** | **265 (0.247)** | **103 (0.096)** | **1,562,355** | **9,221** |

There were 43, 190, and 32 structurally automorphic programs in the
two-thread, three-thread, and fuzz corpora respectively. The conservative
`Signal` rule removed 3, 35, and 14 of those. TSO and PSO each had 26
structurally and admissibly automorphic programs. Maximum identical
multiplicity was three in the three-thread corpus.

The gallery comprises 26 naive/DPOR-paired programs and six DPOR-only
programs. None meet the exact-body criterion, including dining philosophers.

### Orbit counts

The exact orbit counts for the eligible programs were:

| Corpus | E | N | D | M | O | D - O | M - O |
|---|---:|---:|---:|---:|---:|---:|---:|
| two-thread SC | 26 | 101 | 41 | 40 | 26 | 15 | 14 |
| three-thread SC | 29 | 2,964 | 240 | 193 | 79 | 161 | 114 |
| TSO oracle | 20 | 381 | 99 | 74 | 42 | 57 | 32 |
| PSO oracle | 20 | 381 | 99 | 74 | 42 | 57 | 32 |
| fuzz | 8 | 31 | 11 | 11 | 8 | 3 | 3 |
| **Total** | **103** | **3,858** | **490** | **392** | **197** | **293** | **195** |

The focused three-reader rwlock discriminator is an important negative
result. It has 1,680 naive schedules, but current DPOR already explores one
schedule and the class counts are `M = O = 1`. Thread symmetry cannot improve
that program.

Candidate exclusions, counted only where an identical body exists, were:

- two-thread SC: 10 no-admissible-automorphism, 12 model-error/assertion, and
  2 node-sensitive programs;
- three-thread SC: 193 no-admissible-automorphism, 101
  model-error/assertion, and 25 node-sensitive programs;
- TSO and PSO, each: 1 model-error/assertion, 1 too large, and 4
  node-sensitive programs; and
- fuzz: 68 no-admissible-automorphism, 1 incomplete, and 9 node-sensitive
  programs.

The `Signal` counts above are subsets of the no-admissible-automorphism
counts. No candidate was excluded for a cycle, bound, class outcome, class
lower bound, or report-isomorphism violation.

### Where fuzz symmetry occurs

Fuzz has the highest raw candidate prevalence, 86 of 3,000 programs (2.867%),
but only 18 (0.600%) retain an admitted automorphism and only eight enter the
orbit count. Candidate/admissible/total tag counts were:

| Dimension | Candidate / admissible / total |
|---|---:|
| mostly-well-formed | 41 / 17 / 2,400 |
| value | 42 / 0 / 300 |
| adversarial | 3 / 1 / 300 |
| SC | 60 / 17 / 1,876 |
| TSO | 7 / 0 / 748 |
| PSO | 19 / 1 / 376 |
| one thread | 0 / 0 / 43 |
| two threads | 11 / 10 / 2,071 |
| three threads | 49 / 2 / 612 |
| four threads | 7 / 4 / 150 |
| five threads | 19 / 2 / 124 |

There were 84 candidate groups of normalized length one and three of length
two. The generator therefore produces exact duplicates mostly as tiny bodies;
the frequent three- and five-thread candidates usually fail whole-program
automorphism validation.

### Wall-time opportunity

No reducer exists, so a measured speedup would be impossible to report. The
diagnosis instead records the time already spent inside each DPOR exploration
and applies two schedule-linear proxies per eligible program:

```text
gross estimate         = dpor_time * (D - O) / D
symmetry-only estimate = dpor_time * (M - O) / D
```

This deliberately excludes offline orbit analysis time, but it also excludes
all canonicalization, stabilizer, permutation, and witness-translation cost a
real implementation would add. It is an opportunity estimate, not an
empirical speedup.

Three instrumented passes produced the following aggregate nanosecond
measurements and estimates. `I time` covers every exact-body candidate, and
`A time` covers every admitted automorphic program, including programs that
the exact orbit counter excludes.

| Pass | I time | A time | Gross estimate | Symmetry-only estimate |
|---|---:|---:|---:|---:|
| 1 | 169,940,908 | 99,953,491 | 11,696,361 | 6,328,079 |
| 2 | 65,766,830 | 23,757,913 | 1,622,788 | 1,075,129 |
| 3 | 66,106,225 | 22,123,635 | 1,660,166 | 1,084,303 |

The spread confirms that these sub-millisecond program timings are noisy. To
avoid overstating the opportunity, the decision uses the smallest complete
estimate: 0.001622788 seconds gross and 0.001075129 seconds symmetry-only.

The broader timing columns bound what the orbit exclusions could conceal. On
the same selected pass, eliminating every nanosecond spent in all exact-body
candidates would save at most 0.13017979% of gate wall time. Eliminating every
nanosecond in all admitted automorphic programs would save at most 0.04702675%.
Across all three passes those ceilings range from 0.13017979% to 0.33638343%
and from 0.04379183% to 0.19784935%, respectively. Both are deliberately
impossible full-elimination ceilings, not predicted speedups; real symmetry
reduction would retain representatives and add work.

The six uninstrumented selected gates took 83.58, 91.04, and 50.52 seconds in
three serial Release runs. The sum of the independently best per-suite values
was 48.56 seconds, but mixing suites from different runs is not used as the
primary denominator. Against the best complete 50.52-second pass, the modeled
gross saving is 0.00321217% and the symmetry-only saving is 0.00212813% of
gate wall time. Even the gross number is three orders of magnitude below a
one-percent result.

### Reproduction commands

The diagnosis is built only when its option is explicitly enabled. Its focused
test is registered with CTest; the six long-running measurement twins are not.

```sh
cmake -S . -B build-symmetry -DCMAKE_BUILD_TYPE=Release \
  -DDPOR_BUILD_SYMMETRY_DIAGNOSIS=ON
cmake --build build-symmetry --target \
  symmetry_diagnosis_tests \
  dpor_oracle_symmetry dpor_oracle_3threads_symmetry \
  tso_oracle_symmetry pso_oracle_symmetry \
  dpor_fuzz_differential_symmetry classic_gallery_tests_symmetry dpor -j8
ctest --test-dir build-symmetry \
  -R '^symmetry_diagnosis_tests$' --output-on-failure
```

Run the complete corpora as follows. The first six commands emit both the
deterministic `symmetry_diagnosis:` line and a sampled `symmetry_timing:` line.

```sh
./build-symmetry/dpor_oracle_symmetry
./build-symmetry/dpor_oracle_3threads_symmetry
./build-symmetry/tso_oracle_symmetry
./build-symmetry/pso_oracle_symmetry
./build-symmetry/dpor_fuzz_differential_symmetry
repo=$PWD
mkdir -p /tmp/dpor-c13-gallery
(cd /tmp/dpor-c13-gallery && \
  "$repo/build-symmetry/classic_gallery_tests_symmetry" \
  "$repo/build-symmetry/dpor" "$repo")
```

For a count-only reproducibility pass, append this pipeline to each command and
repeat it twice. The two complete passes produced identical lines.

```sh
| rg '^symmetry_diagnosis:'
```

For a timing sample, use the corresponding pipeline:

```sh
| rg '^symmetry_timing:'
```

The uninstrumented wall denominator was reproduced three times with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDPOR_BUILD_SYMMETRY_DIAGNOSIS=OFF
cmake --build build -j8
/usr/bin/time -p ctest --test-dir build \
  -R '^(dpor_oracle|dpor_oracle_3threads|tso_oracle|pso_oracle|dpor_fuzz_differential|classic_gallery_tests)$' \
  --output-on-failure
```

The count lines contain no clocks or unordered iteration. Timing uses
`steady_clock` and is intentionally reported as sampled, nondeterministic
evidence rather than a byte-stable count.

## Stage 2: requirements for a sound implementation

### Preferred shape: preserve original coordinates

A future campaign should first attempt static or prefix-based lex-leader
symmetry breaking while leaving `ExecutionState`, the interpreter, and public
schedules in original thread coordinates. At each prefix, the algorithm would
retain one enabled transition from each valid orbit under the prefix
stabilizer. The proof obligation is that the lex-leader rule retains at least
one extension from every complete schedule orbit and remains compatible with
DPOR repairs. This shape avoids translating a report after exploration.

If that proof cannot be made local to the current backtrack-set algorithm, the
alternative is dynamic orbit canonicalization. Every canonicalized node and
edge must then carry an invertible original-to-canonical permutation. Edge
maps must compose through the trace and invert before any public report is
created. An anonymous canonical state or a schedule without its permutation
is not acceptable.

In either design, canonical choice must be deterministic: ordered candidate
permutations, one documented total order, and no clock, hash-table iteration,
pointer identity, or random tie-break.

### Witness obligations

Every public report has a distinct translation and replay obligation:

- **Race:** restore both endpoint thread/action coordinates and the complete
  witness schedule. The named address and both endpoint actions must agree
  after original-coordinate replay.
- **Deadlock:** restore every blocked thread, mutex owner, join target, wait
  target, and witness step. Sort only after translation, then reconstruct the
  same wait-for relation by replay.
- **Model error:** retain a structured failing endpoint and original schedule.
  Error strings containing numeric thread IDs must be rendered after
  translation; textual digit substitution is not sound.
- **Assertion:** restore the failing thread/action endpoint, register and value
  context, and full schedule, then reproduce the assertion failure by replay.
- **Nontermination:** emit an original-coordinate stem and cycle whose final
  replay state equals the cycle-start state exactly. A canonical closure
  `S_end = permutation(S_start)` is insufficient. One safe construction is to
  repeat the quotient cycle through the permutation's finite order and prove
  exact closure; otherwise the report and CLI grammar must explicitly record
  and replay the closing permutation.
- **Fairness:** recompute weak and strong fairness from the translated,
  exactly closed original witness. Participation and enabled endpoints are
  original thread facts; translating a canonical fairness label alone is not
  enough.
- **Minimized schedules:** translate first, minimize in the original transition
  system, and replay the minimized result against the same report predicate.
  Minimization must not silently discard a required permutation record.
- **CLI round-trip:** existing output should remain an original-coordinate
  schedule accepted by the existing replay command. If an explicit mapping is
  unavoidable, it needs a versioned report and schedule grammar, deterministic
  serialization, parser validation, and replay semantics. An implicit map
  known only to the explorer is forbidden.

First-witness ordering also needs an explicit compatibility decision. A
quotient can change which orbit is visited first even when every reported
witness replays. A future campaign must either preserve the documented
original-coordinate order or predeclare and golden-test the deterministic new
order.

### Exploration interaction audit

The future design must resolve all of the following before implementation:

- **Persistent, backtrack, and done sets:** transition IDs live in a node's
  coordinate system. Sets must be quotiented only by the current stabilizer,
  and race repairs inserted in the same coordinates. A global representative
  chosen after the prefix has broken symmetry can lose a required reversal.
- **Sleep sets:** parent entries must be mapped through the parent-to-child
  permutation. Orbit-equivalent transitions may be slept only under the child
  stabilizer; otherwise all representatives of a live orbit can be suppressed.
- **Disabled-transition repairs:** enabler chains must translate every disabled
  transition and enabling predecessor across node permutations. This includes
  locks, joins, spawn-gated aliveness, sleeping and woken waits, and the
  lowest-waiter rule that caused the conservative `Signal` exclusion.
- **Occurrence identity:** repeated source actions cannot be identified by
  thread and action index alone internally. Wait phases, loop occurrences,
  flush occurrences, and especially barrier generations must survive
  canonicalization. Barrier generation may remain absent from the public
  endpoint while still being mandatory in the internal occurrence key.
- **`TryLock`:** success and failure depend on the current owner, and the
  same-mutex independence refinement is path-sensitive. Owner identity, HB
  effects, and third-holder cases must be permuted before any representative
  is selected.
- **TSO and PSO:** per-thread FIFO buffers, PSO per-address subqueues, buffered
  values, source PCs, and ownership must be permuted with the thread. The flush
  action-index sentinel remains a sentinel. Under thread-only symmetry,
  canonical numeric address IDs and PSO flush-address fields do not change;
  broader resource symmetry would have to map them too.
- **Full state:** a dynamic canonical form must map both thread axes of vector
  clocks, HB clocks, per-thread control state and registers, race metadata and
  stored endpoints, owners, waiter queues, barrier arrivals and generations,
  liveness, and memory-model buffers. The current behavioral fingerprint omits
  data that exact transition and witness semantics require; it cannot be
  reused as an orbit-canonical state.
- **Fingerprint and lasso machinery:** orbit equality must remain distinct from
  exact state equality. A permutation-closed edge is not an ADR 0016 lasso.
  Exact original closure, or an explicit and replayable permutation cycle
  expanded to exact closure, is required before fairness classification.
- **Optimality accounting:** the existing meter must continue to report raw
  `D` and `M`. Quotient-aware results must add a separately named `O` and state
  the admitted group. Replacing the denominator or comparing raw and quotient
  counts without labeling them would make the meter dishonest.

### Complexity

For identical blocks of sizes `k1, k2, ..., kn`, the narrow candidate group has
size:

```text
|G| = product(ki!)
```

Whole-program validation costs `O(|G| * A)` for `A` normalized actions. A
dynamic implementation pays at least `O(|G| * S)` per canonicalized node to
permute, serialize, and compare a state of size `S`, before stabilizer updates,
set normalization, permutation composition, and report recovery. `S` includes
quadratic thread-indexed clock data and all pending buffer entries. Resource
renaming would add a larger automorphism problem rather than reduce this cost.

Against a modeled symmetry-only saving of approximately 0.002% of gate wall
time, even one additional full-state comparison on ordinary nodes would likely
cost more than the measured benefit. That comparison is intentionally
qualitative: only a future prototype can measure canonicalization cost, but
the opportunity bound does not justify building that prototype today.

## Revisit clause and future stop bars

Reopen the decision only when at least one deterministic, replayable workload
meets one of these evidence conditions:

1. admitted exact-body symmetric programs consume at least 10% of total DPOR
   wall time and the same `M - O` model predicts at least 5% total wall-time
   saving over three serial Release runs; or
2. a representative target workload spends at least 20% of its checker time
   in repeated-worker symmetry and has an absolute avoidable cost of at least
   10 seconds; or
3. a broader resource-renaming automorphism is specified formally and a
   deterministic corpus demonstrates at least 5% total modeled saving after
   paying measured canonical-form cost.

If a condition fires, a future campaign must proceed in stages:

1. extend the meter and exhaustive tiny-program oracle with the intended
   automorphism relation and every report kind, without changing exploration;
2. prototype the original-coordinate lex-leader behind a default-off build
   flag, with a proof or exhaustive check that every orbit retains a complete
   representative;
3. run all 27 suites in both build flavors, preserve raw meter lines, add a
   separately labeled quotient meter, and replay every emitted witness through
   the CLI; and
4. consider shipping only if best-of-three end-to-end wall time improves by at
   least 5% on the workload that reopened the decision, with no gate regression
   above 2% outside timing noise.

Stop that campaign immediately if any report cannot be emitted and replayed in
original coordinates or with an explicit validated permutation; if a lasso
closes only canonically; if fairness must be inferred rather than replayed; if
an oracle assertion must be weakened; if a class/report isomorphism check
fails; if output becomes nondeterministic; or if measured canonicalization
overhead erases the predeclared 5% benefit.

## Strongest counterargument

The exact-body definition deliberately undercounts realistic symmetry. Dining
philosophers and workers assigned distinct but interchangeable resources can
be symmetric only under a joint thread/resource permutation. Moreover, the
measured eligible slice loses nearly half its Mazurkiewicz classes under the
thread quotient. A future deployment dominated by replicated workers could
therefore benefit substantially even though the current gates do not.

That is a reason to rerun this diagnosis on such a workload, not to ship the
current idea. Joint resource symmetry must also translate memory addresses,
mutexes, rwlocks, semaphores, barriers, conditions, buffer partitions, and
every resource named in a report. It expands both the proof surface and the
canonicalization cost that already outweigh the current measured opportunity.

## Default-off implementation boundary

`DPOR_BUILD_SYMMETRY_DIAGNOSIS` defaults to `OFF`. When enabled, it builds a
test-only support library, one focused test, and separate twins of the six gate
binaries. Gate-source hooks are guarded by `DPOR_SYMMETRY_DIAGNOSIS`. Normal
targets do not link the support library, start clocks, collect schedules, or
print diagnosis lines. No symmetry representative is selected in any build;
the optional code only post-processes complete existing exploration results.

## Verification status

The canonical pre-change smoke passed 27/27 suites. After all diagnosis and
documentation changes, the default-off Release build passed 27/27 suites in
94.19 seconds. A Debug build with `DPOR_ENABLE_RESTORE_ASSERTS=ON` also passed
27/27; parallel CTest scheduling completed in 185.62 seconds. The optional
focused diagnosis test passed, all six instrumented twins built, and two
complete corpus passes produced byte-identical `symmetry_diagnosis:` lines.

Fresh `dpor_optimality` output retained the complete byte-identical meter
lines, including the pinned ratios SC 1.067, TSO 1.152, and PSO 1.154. Two
verifier-written, label-shifted exact-body CLI probes also round-tripped:

- two unprotected identical writers reported a race at original endpoints
  `(thread 0, action 1)` and `(thread 1, action 2)`; replay of schedule
  `0 1; 1 2` reproduced the same payload; and
- two identical double-lock bodies reported both original threads blocked on
  mutex `m` owned by thread 0; replay of schedule `0 1` reproduced the same
  deadlock payload.

The temporary probe files were removed after verification. `git diff --check`
was clean, and no commit was created by this campaign run.

## Reviewer scrutiny

Review should concentrate on these claims rather than the headline percentage:

- label normalization, empty-body handling, and `Spawn`/`Join` target mapping;
- the lowest-waiter `Signal` restriction and the distinction between structural
  and admitted automorphisms;
- every orbit-meter exclusion, especially cycles, errors/assertions,
  `TryLock`, barriers, incomplete fuzz cases, and the 20,000-schedule cap;
- the arithmetic boundaries `O <= M <= D <= N`, the separation of `D - O`
  from `M - O`, and the absence of report-isomorphism violations;
- the use of complete-pass wall time and the description of the timing result
  as a schedule-linear proxy rather than an observed speedup;
- exact lasso closure, fairness recomputation, and CLI translation in the
  future-design section; and
- the default-off CMake and macro boundary, including that the heavy
  measurement twins are not registered in the normal CTest suite.

## Invariants protected

- **Replayability:** no quotient is shipped without an original-coordinate or
  explicitly mapped witness for every report type.
- **Exact lasso closure:** equality modulo permutation is never substituted for
  the interpreter's exact cycle closure.
- **DPOR soundness:** persistent, sleep, and disabled-repair interactions remain
  unchanged until their symmetry semantics are proved.
- **Determinism:** count measurements use ordered canonical forms; sampled
  clocks appear only on separately labeled timing lines.
- **Measurement honesty:** exact-body prevalence, admitted automorphisms,
  eligibility, raw DPOR redundancy, symmetry-only classes, and wall-time proxy
  are reported as distinct quantities.
- **Shipped behavior:** the diagnosis option is default-off and no production
  explorer performs symmetry reduction.
