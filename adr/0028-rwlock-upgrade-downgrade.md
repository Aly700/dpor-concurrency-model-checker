# ADR 0028: Reader-writer lock upgrade and downgrade

## Status

Accepted and verified for Campaign 14.

The implementation, deliberate happens-before mutations, complete Release and
restore-assert suites, byte-identical optimality meters, fresh CLI probes,
oracle/fuzz/inclusion totals, gallery round trips, and interleaved timing
comparison all satisfy the predeclared acceptance bar. The verification
section records the final evidence.

## Context

ADR 0021 introduced non-reentrant reader-writer locks with `RLock`,
`RUnlock`, `WLock`, and `WUnlock`. A lock has a canonical reader-holder set,
an optional writer owner, a writer-release clock, and a reader-release
accumulator. Readers acquire only the last writer publication. A writer
acquires both the last writer publication and every accumulated reader
release, then consumes the reader accumulator. This deliberately creates no
reader-to-reader happens-before edge.

Campaign 14 adds atomic mode conversion:

- `Upgrade` converts an existing read hold to a write hold. It waits until the
  caller is the sole reader and never exposes an unheld state.
- `Downgrade` converts an existing write hold to a read hold. It never waits
  and likewise never exposes an unheld state.

These are not aliases for `RUnlock; WLock` or `WUnlock; RLock`. Such
expansions would introduce an acquisition window in which another thread
could win the lock, and they would publish or consume the wrong clock
frontiers.

Upgrade also invalidates an apparently local Campaign 7 reduction argument.
Two same-name `RLock` transitions still commute in their final state, but a
pending next `Upgrade` can observe the intermediate state:

```text
T0: RLock(r); Upgrade(r); ...
T1: RLock(r); Upgrade(r); ...
```

After only T0's acquisition, T0 is the sole reader and its `Upgrade` is
enabled. After both acquisitions, neither conversion is enabled. Reversing
the two acquisitions exposes the symmetric T1-first conversion. The
intermediate transition was not enabled at the root, so an action-only
`RLock`/`RLock` independence result can let persistent- or sleep-set pruning
discard an upgrader-first class even though the two-acquisition final states
match.

## Decision

### Actions, ownership, syntax, and misuse

Append `Upgrade` and `Downgrade` to `ActionKind`, after every pre-existing
kind, and append `RwLockUpgrade` to `BlockedOnKind`. Appending preserves every
pre-existing numeric identity as compatibility hygiene; the diagnostic and
optimality corpora otherwise remain unchanged.

The strict source and rendering spellings are:

```text
upgrade RWLOCK
downgrade RWLOCK
```

They use the existing rwlock namespace. A rwlock name remains distinct from
mutex, semaphore, and barrier names; those inherited collisions are rejected
by both the CLI loader and direct `Program` construction.

For rwlock `r`, let `Readers(r)` be its reader-holder set and `Writer(r)` its
optional writer owner.

`Upgrade(r)` by thread `t` is a valid acquisition only when:

```text
t in Readers(r)
Writer(r) is empty
Readers(r) == {t}
```

If `t` has a read hold but another reader remains, the action is disabled and
retains that read hold. If `t` has no read hold, including when it holds write
mode, the action remains executable so the forward interpreter reports the
same deterministic modeled-misuse shape as a mismatched rwlock unlock.

A successful upgrade performs this ownership change in one source
transition:

```text
Readers(r)' = Readers(r) \ {t} = {}
Writer(r)' = t
```

`Downgrade(r)` is valid only when `Writer(r) == t`. A valid downgrade is never
blocked and performs:

```text
Writer(r)' = empty
Readers(r)' = Readers(r) union {t} = {t}
```

No scheduling point exists between either pair of ownership updates. Thus no
other thread can observe the lock as unheld during a conversion.

Invalid conversion follows the existing mismatched `RUnlock`/`WUnlock`
convention exactly: the source transition ticks and advances its pc, reports a
modeled error, and does not mutate rwlock ownership or either rwlock release
clock. The stable messages are:

```text
thread T attempted to upgrade rwlock 'R' but it does not hold that mode
thread T attempted to downgrade rwlock 'R' but it does not hold that mode
```

Campaign 7's legacy `WLock`-while-holding-read behavior is unchanged. It is a
permanent self-wait reported through `RwLockReaders` and `self_wait`; it is
not reinterpreted as the new atomic conversion.

### Happens-before equations

For one rwlock `r`, define:

- `W_r` as its `writer_release` clock;
- `R_r` as its `reader_releases` accumulator;
- `C_t` as thread `t`'s clock before a conversion; and
- `C_t+ = tick_t(C_t)` as the common post-tick action clock.

The Campaign 7 lifecycle remains:

```text
RLock:   C_t' = C_t+ join W_r
RUnlock: R_r' = R_r join C_t+
WLock:   C_t' = C_t+ join W_r join R_r; R_r' = empty
WUnlock: W_r' = C_t+
```

The two conversions extend that lifecycle as follows.

Successful `Upgrade` consumes exactly the same publication frontiers as
`WLock`:

```text
C_t' = C_t+ join W_r join R_r
R_r' = empty
W_r' = W_r
Readers(r)' = {}
Writer(r)' = t
```

Every other reader that drained before the upgrade contributed its post-tick
release to `R_r`, so those releases happen-before the upgrader's write
section. The caller's live read hold is never released into `R_r`; it
therefore contributes no artificial self-release edge. Clearing `R_r` starts
the next reader epoch exactly as a successful `WLock` does. A later
`WUnlock`, or a later `Downgrade`, republishes the resulting writer clock.

Successful `Downgrade` publishes at the conversion point while retaining the
resource in read mode:

```text
C_t' = C_t+
W_r' = C_t+
R_r' = R_r
Writer(r)' = empty
Readers(r)' = {t}
```

Replacement, rather than a join into the old `W_r`, matches `WUnlock`; the
writer's current clock already contains every frontier it acquired. Every
subsequent `RLock`, `WLock`, or `Upgrade` acquirer joins `W_r`, so writes in
the downgraded writer section are visible. Downgrade does not consume or clear
`R_r`. The retained reader's eventual `RUnlock` contributes its then-current
post-tick clock to that accumulator in the ordinary way.

Nothing changes `RLock`'s rule: it joins `W_r` and never `R_r`. In particular,
neither conversion creates a reader-release-to-reader-acquire edge. A
negative leak probe keeps a conflicting plain access by two readers racy.

### TSO and PSO ordering

Both conversions are ordered synchronization points under TSO and PSO, with
the same explicit-drain rule as the four existing rwlock operations. A source
`Upgrade` or `Downgrade` is disabled while its thread owns any pending store
buffer entry. The model exposes the existing flush transitions; conversion
performs no hidden flush.

Focused replay probes cover both operations under both buffered models:
`RLock; Write; Upgrade` and `WLock; Write; Downgrade` reject a conversion
scheduled before the explicit drain and accept it after the corresponding
TSO or PSO flush.

### Upgrade deadlock identity

A disabled valid `Upgrade` is waiting for other readers, not for the caller's
retained read hold. It is reported as:

```text
BlockedOnKind::RwLockUpgrade
rwlock R upgrade_waiting_for_readers_to_drain
```

The blocker has neither an owner nor `self_wait`. The caller's retained hold
is the required conversion state; only the other live readers prevent
enabledness.

Consequently, two threads that both hold read mode and both wait at
`Upgrade(r)` are permanently blocked on one another. A barrier-synchronized
fixture forces this state and pins two `RwLockUpgrade` entries, stable replay
identity, six naive schedules, and four DPOR schedules. It must be a deadlock,
not a wrong-mode error or clean termination. The CLI `check`/`replay` form
must preserve the same two rendered blocker lines byte-for-byte.

### DPOR dependence and the Upgrade-sensitive restriction

The public `independent(Action, Action)` relation is conservative for every
same-name rwlock pair. This includes `RLock`/`RLock` and every pair containing
`Upgrade` or `Downgrade`. Operations on distinct rwlocks retain the existing
disjoint-resource independence, subject to the cross-cutting thread, spawn,
join, and buffered-transition rules.

The checker restores the Campaign 7 `RLock`/`RLock` reduction only with
whole-program, per-name knowledge:

```text
cross-thread RLock(r) / RLock(r) is independent
iff the complete static program contains no Upgrade(r)
```

Absence is scoped to the named rwlock; an `Upgrade` on another name does not
disable this result. When no `Upgrade(r)` exists, neither acquisition can
expose the sole-reader conversion witness. The original diamond then closes:
both orders leave the same canonical reader set, writer owner, release
clocks, per-thread clocks, values, race state, and enabled set.

Beyond that exact acquisition/acquisition rule, the broader checker-local
reader-mode refinement remains narrower. A cross-thread reader-mode pair
involving at least one `RUnlock(r)` is commuted only when the complete program
contains none of:

```text
WLock(r), WUnlock(r), Upgrade(r), Downgrade(r)
```

This is the statically writer-mode-free rule. In particular, with an upgrader
already holding read mode, `RUnlock; Upgrade(third); RLock` can expose a
middle conversion that the opposite order keeps disabled. Excluding every
writer-mode action removes that observer and preserves the reader-only
holder-set and release-join diamond. No optional `RUnlock`/`RUnlock`
refinement is added outside this existing writer-mode-free proof.

All conversions remain dependent with every same-name operation even where a
smaller local state equation might appear to commute. This deliberately
protects ownership, HB publication, misuse outcomes, and middle enabledness.

The DPOR machinery audit is:

1. **Persistent closure.** Same-name conversions are dependent with every
   same-name operation, so a persistent set closes over a co-enabled
   conversion or resource transition. The only restored same-name
   `RLock`/`RLock` result is guarded by static absence of the transition that
   breaks its middle-state diamond.
2. **Sleep inheritance.** A slept transition is inherited only if the same
   numeric endpoint and effective action survive enabled in the child and
   commute at the parent node. If one `RLock` makes a sole-reader `Upgrade`
   current and the next `RLock` disables it, that conversion occurrence is
   absent from the child enabled map and cannot remain asleep.
3. **Disabled-transition repair.** A blocked `Upgrade` retains the caller's
   reader hold, so repair skips that caller and recursively follows every
   other reader's current enabler head. An enabler cycle, including two
   mutually blocked upgraders, or any chain that cannot be proved returns to
   the established all-enabled conservative fallback. Legacy `WLock`
   self-wait repair continues to use that fallback instead of skipping self.
4. **Occurrence identity.** A successful or invalid conversion advances its
   source pc exactly once. A blocked `Upgrade` does not fire. Therefore
   `(thread, action_index)` plus the exact enabled action identifies a
   conversion occurrence; no phase or generation stamp is needed.
5. **Terminal endpoints.** Wrong-mode conversions terminate as modeled
   errors. The existing terminal safeguard clears sleep and backtracks every
   enabled sibling, so a terminal misuse is not used as one side of a
   successful commuting diamond.

An asymmetric discriminator pins the reason for the static restriction. One
thread upgrades and writes a flag; the other upgrades first, reads the old
flag, and fails an assertion. Acquiring both read holds instead produces the
conversion deadlock. Naive and DPOR must preserve both the assertion class and
the deadlock class, with replayable reports.

### Behavioral fingerprints and ADR 0023 elision

No behavioral fingerprint field is added.

The existing normalized pc, canonical reader-holder set, and optional writer
owner already distinguish every conversion-relevant state:

- a blocked upgrade retains its source pc and all live readers;
- a fired upgrade has an advanced pc and the caller as writer;
- a pre-downgrade state has the caller as writer at the downgrade pc; and
- a fired downgrade has an advanced pc and the caller in the reader set.

`W_r` and `R_r` remain HB analysis instrumentation and stay outside the
behavioral fingerprint, like mutex release clocks. Conversion does not make
either clock program-observable or enabledness-relevant independently of the
already fingerprinted ownership and control state.

ADR 0023's history-elision proof extends without a new exceptional action. A
blocked `Upgrade` executes no transition and can only contribute to a
terminal deadlock until another source action changes ownership. Every fired
`Upgrade` and every fired `Downgrade`, including misuse, advances normalized
pc. Under TSO/PSO any prerequisite flush strictly drains a finite buffer under
the existing measure. Thus an acyclic program containing conversions cannot
repeat a behavioral state through a same-pc conversion. A retry requires the
already detected self/backward branch that enables exact cycle history.

### Text, gallery, and acceptance gates

The parser, renderer, namespace audit, oracle diagnostics, fuzz diagnostics,
and optimality diagnostics recognize both new actions. The optimality corpus
itself is unchanged.

The required gallery pair is:

- `rwlock_upgrade_correct.dpor`: read, atomically upgrade, write, downgrade,
  and release, with a transient peer reader; it is clean.
- `rwlock_upgrade_double_deadlock.dpor`: two barrier-synchronized readers
  both upgrade; it produces the exact two-blocker deadlock.

Both have stored DPOR CLI stdout goldens. The deadlock variant is checked
under naive exploration, DPOR, CLI check, and CLI replay; the correct variant
is checked as clean and against its golden.

The acceptance corpora are widened as follows:

- the SC two-thread and three-thread alphabets contain 27 and 25 actions,
  respectively, including structurally guarded `Upgrade` and `Downgrade`
  entries;
- the TSO and PSO alphabets contain 19 actions, including all six rwlock
  operations and valid buffered conversion fixtures, with zero capped skips;
- deterministic differential fuzz generates valid read/write mode toggles
  and adversarial misuse, reports generated and fully compared conversion
  counts per lane, requires at least 300 generated actions of each conversion
  in aggregate, and requires at least 75 percent of each lane/operation's
  generated conversions to remain in fully compared programs;
- cross-model inclusion adds both actions to its enumerated and fuzz
  alphabets and compares four dedicated conversion programs under SC, TSO,
  and PSO with zero conversion skips.

The HB gate contains both thread-ID directions for each positive edge:

1. a downgraded writer's plain write must happen-before a later reader's
   conflicting read through `W_r`;
2. a prior reader's plain access must happen-before the upgrader's conflicting
   write through `R_r`; and
3. two plain readers must retain a race, proving that no accumulator edge
   leaked into `RLock`.

The positive probes are required to be verdict-flipping. Verification must
temporarily remove only the `Downgrade` assignment to `writer_release`; both
downgrade-publication probes must then expose the missing edge. After
restoring it, verification must remove only the `Upgrade` join of
`reader_releases`; both upgrade-accumulator probes must expose that missing
edge. The source must be restored after each mutation, and the negative
reader-reader probe must remain racy. A passing unmutated test alone is not
accepted as proof that either positive fixture discriminates its intended
clock update.

The unchanged optimality corpus must continue to print exactly:

```text
SC 1.067
TSO 1.152
PSO 1.154
```

Final acceptance also requires the complete Release suite and the Debug suite
with `DPOR_ENABLE_RESTORE_ASSERTS=ON`, fresh adversarial CLI check/replay
probes, and an interleaved best-of-three full-suite timing comparison against
pristine commit `b432cdc`, following ADR 0023's method. No meter rebaseline,
skipped gate, hidden flush, or weakened class guard is permitted to satisfy
those checks.

## Verification

The untouched pre-change Release smoke test passed 27 of 27 tests. The
assembled campaign passed 28 of 28 tests in both required flavors:

```text
Release:  cmake --build build -j8 && ctest --test-dir build
Debug:    cmake --build build-restore -j8 &&
          ctest --test-dir build-restore
          (CMAKE_BUILD_TYPE=Debug, DPOR_ENABLE_RESTORE_ASSERTS=ON)
```

The first complete Debug run took 379.53 seconds; the final post-review run
took 294.69 seconds. Both included the restore-assert oracle, gallery, fuzz,
and conversion tests. No pre-existing test was removed or altered to avoid a
failure.

The focused semantic gates pin:

- transient-reader schedules at 4 naive and 4 DPOR;
- double-upgrade deadlock schedules at 6 naive and 4 DPOR, with the same two
  `RwLockUpgrade` blockers and witness through both explorers, CLI check, and
  replay;
- wrong-mode conversion bytes and replay identity;
- explicit pre-drain rejection and post-flush success for both conversions
  under both TSO and PSO; and
- the Upgrade-bearing static restriction, its per-name scope, and the
  asymmetric assertion/deadlock discriminator.

The positive HB fixtures were proven verdict-flipping, not merely passing.
Removing only Downgrade's assignment to `writer_release` made each directional
downgrade-publication probe expose the expected race. After restoring that
line, removing only Upgrade's `reader_releases` join did the same for each
directional upgrade-accumulator probe. For each pair, the first assertion was
temporarily bypassed only to execute the mirrored assertion. Production and
test source were restored after each mutation; the complete unmutated HB gate
then passed, and the negative plain-reader probe remained racy.

The widened deterministic gates reported:

| Gate | Programs | Alphabet | Naive schedules | DPOR schedules | Skips |
|---|---:|---:|---:|---:|---:|
| SC two-thread | 22,736 | 27 | 59,119 | 34,419 | 0 |
| SC three-thread | 65,547 | 25 | 789,230 | 331,415 | 0 |
| TSO | 11,365 | 19 | 65,287 | 20,781 | 0 |
| PSO | 6,246 | 19 | 41,875 | 11,128 | 0 |

Fixed-seed differential fuzz generated 3,000 programs, compared 2,978, and
reported 22 capped programs. It generated/compared 317/317 Upgrades and
363/363 Downgrades: the mostly-well-formed lane supplied 23/23 and 35/35,
while the adversarial lane supplied 294/294 and 328/328. Cross-model inclusion
compared all 1,731 programs with zero skips and passed 17,310 checks; its
dedicated rwlock-conversion corpus was 4 attempted, 4 compared, 0 skipped.

The unchanged optimality binary printed the required byte-identical meter
lines:

```text
SC 1.067
TSO 1.152
PSO 1.154
```

Both gallery files matched their stored stdout goldens. Fresh temporary-file
CLI probes independently exercised Upgrade HB (5 naive / 4 DPOR / 1 replay),
Downgrade HB (5 / 5 / 1), and double-upgrade deadlock (6 / 4 / 1), preserving
the exact verdict, trace, schedule, and blocker bytes. Their scratch files were
moved outside the worktree after verification.

Finally, a pristine `git archive` of baseline `b432cdc` was built with the
same Release flags. Six full suites ran in the accepted interleaving
`baseline, campaign, campaign, baseline, baseline, campaign`; every baseline
run passed 27/27 and every campaign run passed 28/28:

| Full Release `ctest` wall time | Sample 1 | Sample 2 | Sample 3 | Best |
|---|---:|---:|---:|---:|
| Baseline `b432cdc` | 35.74s | 38.73s | 46.23s | 35.74s |
| Campaign 14 | 54.00s | 33.36s | 70.96s | 33.36s |

The campaign best is 6.7 percent faster despite the added suite, so the
no-wall-time-regression gate passes. The spread confirms why ADR 0023 records
best-of-three rather than a single host-load-sensitive sample.

## Consequences

- Programs can convert read ownership to write ownership, and write ownership
  to read ownership, without exposing an unlocked interval.
- Upgrade preserves all prior reader-to-writer and writer-to-writer
  publication; downgrade publishes the writer section immediately while
  preserving the ordinary later reader release.
- Two competing upgraders are reported as a stable, resource-specific
  deadlock rather than misuse.
- Same-name public dependence is more conservative than Campaign 7.
  Checker-local `RLock`/`RLock` reduction is recovered exactly where static
  absence of same-name `Upgrade` closes the missing middle-state diamond.
- The broader reader-mode optimization remains available only for a
  statically writer-mode-free rwlock.
- No conversion phase, generation, schedule field, or behavioral fingerprint
  field is introduced.
- TSO and PSO preserve explicit buffer visibility: conversion waits for
  drains and never performs one implicitly.

## Invariants protected

- **Happens-before:** Upgrade consumes all prior reader releases without a
  live-hold self-release; Downgrade publishes the post-tick writer clock; and
  `RLock` still consumes no reader accumulator.
- **Atomic ownership:** every valid conversion is one source transition with
  no scheduler-visible unheld state.
- **Independence soundness:** public same-name dependence, the per-name
  Upgrade-free `RLock` guard, and the writer-mode-free reader rule preserve
  both final-state and intermediate-enabledness diamonds.
- **Deadlock soundness:** a valid blocked Upgrade names the rwlock conversion
  wait and repair follows other readers or falls back conservatively.
- **Replay:** conversion endpoints, misuse bytes, blocker identity, and
  explicit weak-memory drains are deterministic under naive, DPOR, and CLI
  replay.
- **Cycle soundness:** existing ownership plus normalized pc fingerprints
  every conversion state, while fired conversions preserve ADR 0023's
  well-founded acyclic measure.
