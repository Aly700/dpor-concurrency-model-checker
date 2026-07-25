# ADR 0030: Mesa timed wait with nondeterministic logical timeout

## Status

Accepted.

Campaign 16 adds a bounded-wait escape to the existing Mesa condition-variable
model without adding wall-clock time, deadlines, or scheduler step counting.
A parked timed waiter has an explicit timeout transition that remains enabled
until either it or a same-condition wake wins. Timeout writes `0`; Signal or
Broadcast wake writes `1`. Both outcomes still reacquire the mutex before the
source pc advances.

The timeout path creates no condition-variable happens-before edge. The wake
path retains the existing Signal/Broadcast edge. Same-condition timeout and
wake operations remain conservatively dependent, and each dynamic park,
timeout, and wake target carries exact episode identity for DPOR's historical
matching boundaries.

## Context

The existing instruction

```text
wait CONDITION MUTEX
```

implements Mesa semantics under one stable numeric endpoint:

1. release the owned mutex and join the sorted parked set;
2. remain disabled until Signal or Broadcast removes the waiter and joins the
   waker's clock into it; and
3. contend for and reacquire the mutex before advancing the source pc.

An empty Signal or Broadcast is forgotten. A later plain Wait therefore parks
forever unless another wake remains reachable, and that terminal state is a
condition-variable deadlock.

Real timed waits add a second resolution: the deadline may expire before a
wake. Modeling elapsed time directly would make behavior depend on a wall
clock, host scheduling, an arbitrary step count, or a numeric deadline with no
clock semantics. None belongs in this deterministic small-step machine.
Model checking instead needs both resolution orders as explicit replayable
schedule choices.

The new transition is the first permanently enabled nondeterministic choice
owned by a parked thread. It can be co-enabled indefinitely with
same-condition Signal/Broadcast and with other timed waiters' timeouts. That
changes deadlock, fairness, occurrence identity, and DPOR history even though
the timeout touches no shared memory.

## Decision and design

### Surface and logical time

The canonical source spelling is:

```text
timedwait CONDITION MUTEX -> rN
```

`CONDITION` uses the condition-variable namespace. `MUTEX` uses the mutex
namespace exactly as Wait, Lock, TryLock, and Unlock do. The destination is
one of the existing eight thread-local registers.

There is deliberately:

- no duration operand;
- no deadline value;
- no wall-clock query;
- no transition-count threshold; and
- no hidden random choice inside execution.

Expiration is represented by an ordinary enabled scheduler transition. A
numeric schedule remains a sequence of the existing
`(thread, action_index[, PSO flush address])` endpoints.

### State machine, result, and replay

TimedWait reuses the existing per-thread Wait phase. Let the source endpoint be
`e = (thread, action_index)`.

**Park.** From the normal phase, executing `e` requires the caller to own the
named mutex. It performs the same release update as Unlock, removes ownership,
inserts the thread into the condition's canonical sorted parked set, and moves
to the waiting phase. The source pc does not advance.

**Timeout resolution.** While that exact episode is parked, `e` remains
enabled as a timeout transition. Executing it removes the thread from the
parked set, writes `0` to the destination register, and moves to the
reacquisition phase. It does not advance the pc.

**Wake resolution.** A same-condition Signal or Broadcast may instead remove
the episode from the parked set. The wake writes `1`, joins the waker's clock
as described below, and moves the waiter to the same reacquisition phase.
There is no separate waiter-owned wake step; resolution occurs as part of the
Signal/Broadcast transition.

**Reacquisition.** The waiter becomes enabled when the mutex is free. Its
effective action is Lock at the original endpoint. It acquires and joins the
mutex release clock, clears the Wait phase, and advances the normalized pc.

The timeout replay therefore executes the TimedWait endpoint three times:
park, timeout, reacquire. A wake replay executes it twice, with the intervening
Signal or Broadcast deciding the result. CLI traces render these phases as
`(sleep)`, `(timeout)`, and `(reacquire)` without changing numeric schedules.
Branches and assertions consume the `0`/`1` result through the existing
register semantics.

### Happens-before

Let `C_w` be the parked waiter's clock and `C_s+` the waker's post-tick clock.

For a Signal or Broadcast wake:

```text
C_w' = C_w join C_s+
result = 1
```

This is the existing Wait wake edge. Broadcast still applies it independently
to every target, with no waiter-waiter edge.

For timeout:

```text
C_w' = C_w
result = 0
```

The timeout transition ticks its own thread as every scheduled transition
does, but performs no condition-variable join. Its only later synchronization
is the ordinary mutex-clock join when reacquisition succeeds.

The no-edge rule is bidirectional:

- a timed-out waiter does not inherit a same-condition signaler's clock; and
- a later Signal or Broadcast does not inherit the timed-out waiter's clock.

Mirrored positive probes make the wake edge the only path from a signaler's
write to the waiter's post-reacquisition read. Bidirectional negative probes
retain races when the timeout path is selected. Separate mutexes prevent an
ordinary mutex handoff from masking either result.

### Deadlock and lasso semantics

A parked TimedWait is never itself a deadlock blocker because its timeout
endpoint is enabled. If every unfinished thread is parked in TimedWait, the
machine still has schedule choices and exploration continues.

After timeout or wake, mutex reacquisition can block normally. A terminal
reacquisition reports the existing mutex blocker and owner, not a
condition-variable blocker. Plain Wait is unchanged: a lost wake can still
produce a condition-variable deadlock.

A backward branch can form the exact cycle:

```text
park -> timeout -> reacquire -> re-park
```

The canonical single-thread timeout spin is a `fair divergence`: its only
unfinished thread owns every cycle step. If a separate Signal endpoint remains
enabled at every cycle state but is never scheduled, the same timeout-spin
witness is an `unfair-schedule witness` under ADR 0026's unchanged weak
predicate. The classification remains inter-thread scheduler fairness; it
does not claim action fairness among choices owned by the participating timed
waiter.

### TSO and PSO

TimedWait follows Wait's existing buffered-model treatment. It is a full
ordered point under TSO and PSO and cannot park while the caller has pending
stores. Only explicit flush transitions drain those stores.

Once parked, the thread cannot enqueue another write before resolution, so the
permanently enabled timeout does not conceal a drain. Timeout touches no shared
memory, performs no hidden flush, and creates no condition synchronization.
The ordered-point classification is a buffered-enabledness rule, not an HB
edge: timeout itself is not a synchronization point.
The later effective Lock phase uses the ordinary ordered mutex-acquisition
treatment. A buffered signaler likewise must drain explicitly before its
Signal or Broadcast can wake the waiter.

### Conservative dependence

The public action relation treats TimedWait as both a condition-variable action
and a mutex action.

- TimedWait timeout versus same-condition Signal or Broadcast is dependent.
  Reordering changes whether the thread belongs to the wake target set and
  whether its result is `0` or `1`.
- Two TimedWait occurrences on the same condition remain dependent.
- Same-mutex pairs remain dependent because park and reacquisition mutate
  ownership and enabledness.
- Distinct condition and mutex names may commute subject to the existing
  thread, spawn/join, and buffered-transition safeguards.

Two already parked timeout transitions for different waiters have a local
adjacent diamond: either order removes the same two members, writes `0` into
disjoint thread-local registers, and leaves both in reacquisition. Campaign 16
does not promote that local observation to an independence claim. No complete
proof covers persistent closure, a middle Signal/Broadcast, historical
matching, disabled repair, and sleep inheritance with exact episodes. The
conservative same-condition relation is the stop-clause outcome and is part of
the accepted design.

### Exact occurrence identity

One thread may park repeatedly at the same static TimedWait endpoint. Numeric
endpoint and static action identity therefore do not identify which parked
episode a timeout ends.

Every TimedWait park and timeout occurrence carries:

```text
(transition = park|timeout, episode)
```

The episode is the caller's checked per-thread executed-step ordinal for that
park. Park predicts the ordinal it is about to create; timeout reuses the
parked episode's ordinal. Reacquisition is already distinguishable as an
effective Lock and carries no TimedWait occurrence component. The public
effective-trace helper exposes this exact phase and episode for verification.

Signal and Broadcast occurrence identity is strengthened from a thread-only
Broadcast waking set to exact dynamic wake targets:

```text
(thread, wait action_index, episode)
```

Signal carries an engaged vector of zero or one target. Broadcast carries an
engaged vector of every target in canonical waiter order, including the empty
vector. The existing public Broadcast waking-thread vector remains available;
the stronger target identity is internal DPOR analysis metadata.

The complete occurrence bundle participates in:

1. enabled and executed transition records;
2. matching current transitions against DPOR nodes;
3. backtrack insertion and persistent-set closure;
4. disabled-transition repair;
5. checker-local state-sensitive independence; and
6. sleep-set inheritance.

Conservative same-condition dependence prevents a timeout, park, Signal, or
Broadcast from crossing a same-condition sleep edge. Exact identity separately
protects cyclic historical matching: returning to the same public endpoint
must not make a new parked episode or wake target equal to an old occurrence.
The ordinal is analysis identity only and is excluded from behavioral cycle
state.

### Behavioral fingerprint and acyclic elision

TimedWait adds no behavioral-fingerprint field.

Fingerprint comparison occurs within one fixed Program. A normalized pc
therefore identifies the static action at that position; pc plus Wait phase
distinguishes a plain parked Wait from a parked TimedWait, including programs
that contain both at different endpoints. Existing fields already record:

- the condition's canonical waiter set;
- the per-thread Wait phase;
- mutex ownership;
- the destination register result; and
- the normalized pc after reacquisition.

Wake clocks, per-thread step ordinals, TimedWait episode stamps, and exact wake
targets are analysis or history state. Including an absolute episode ordinal
would prevent a genuine timeout-spin behavioral state from recurring and
would make lasso detection unsoundly incomplete.

ADR 0023's acyclic-elision proof extends with a three-step measure. Park changes
fingerprinted ownership, waiter membership, and phase. Timeout changes waiter
membership and phase even if the destination already held zero. Reacquisition
changes ownership and phase and advances the normalized pc. A wake instead
requires a Signal or Broadcast source transition that advances the waker's pc,
after which the waiter reacquisition advances its own pc. Without a normalized
self/backward branch, no path can return to the pre-park complete behavioral
state. The timeout is therefore a real phase-advancing machine transition even
though it does not advance source pc.

### Gallery

The classic gallery contains a matched Mesa pair:

```text
mesa_timedwait_bounded_consumer.dpor
mesa_timedwait_bounded_consumer_broken_plain_wait.dpor
```

Both deliberately lose a notification before spawning the consumer. The
TimedWait variant retries once, then executes an explicit fallback and finishes
cleanly. The plain-Wait control parks forever and reports the condition
deadlock. Their DPOR goldens pin one clean and one deadlocking schedule.

## Verification

### Focused semantics and class discriminators

The thirtieth suite pins:

- strict three-phase timeout replay with result `0`;
- Signal and Broadcast wake replay with result `1`;
- exact public park/timeout episode identity and effective-Lock reacquisition;
- a one-signaler/one-waiter winner fixture with both result classes at exactly
  4 naive / 4 DPOR schedules;
- a forced lost-wakeup differential at 1 clean timed schedule versus 1 plain
  Wait deadlock schedule under both explorers;
- blocked post-resolution reacquisition as a mutex deadlock;
- conservative same-condition dependence;
- repeated TimedWait endpoint fixtures in both thread-ID layouts; and
- explicit-drain behavior under TSO and PSO.

The single-thread timeout-spin test pins the exact lasso stem and four-step
cycle under naive exploration, DPOR, and replay. It has no deadlock, one fair
cycle, and the exact label `fair divergence`. A separate replay keeps a
same-condition Signal continuously enabled and pins
`unfair-schedule witness`.

Result-value guards are exact rather than truthiness-only. Timeout preloads its
destination with `1` and must overwrite it with `0`; deleting the core write
makes the focused suite fail. Signal and Broadcast compare the returned value
against an atomic cell containing exactly `1`; a temporary core mutation from
`1` to `2` also fails. The timeout-lasso assertions remain enabled in Release,
and a temporary expected-count mutation from one fair cycle to two fails that
configuration.

Positive wake-HB probes are mirrored across thread IDs. Timeout-negative probes
cover both forbidden clock directions and mirror the timed waiter and signaler
layouts. All reports replay identically.

Suppressing public TimedWait phase metadata makes the explicit effective-trace
fixture fail immediately. A separate exact replay pins park/timeout episodes
`3` and `8` at one numeric endpoint.

The stronger mutation changes only the internal
`TransitionOccurrenceIdentity` TimedWait component to absent. Runtime
semantics and public replay metadata remain intact. The three-thread fixture
has a two-episode TimedWait, one same-condition Signal, and an independent
reader, mirrored so the waiter is `LOW` and `HIGH`:

| Layout | Naive | Exact occurrence DPOR | Stamp-suppressed DPOR |
|---|---:|---:|---:|
| `LOW` | 269 | 22 | 18 |
| `HIGH` | 269 | 38 | 34 |

Both forms still expose race, assertion, and modeled-error shapes. The
mutation therefore witnesses unproved class-accounting loss, not a demonstrated
existential-verdict false negative. The exact identity remains required because
no proof establishes that the discarded representatives are equivalent. The
mutation is restored after the discriminator.

### Widened deterministic gates

All four oracle alphabets contain TimedWait behind explicit presence and
valid-resource guards:

| Gate | Programs | Alphabet | Naive schedules | DPOR schedules | Skips |
|---|---:|---:|---:|---:|---:|
| SC two-thread | 22,903 | 28 | 64,582 | 35,705 | 0 |
| SC three-thread | 65,547 | 26 | 770,547 | 339,177 | 0 |
| TSO | 11,877 | 23 | 54,022 | 19,980 | 0 |
| PSO | 6,707 | 23 | 30,045 | 10,882 | 0 |

The TSO and PSO gates report zero capped skips. Fair, strongly-unfair, and
weakly-unfair cycle-existence comparisons remain non-vacuous in both
explorers.

Fixed-seed differential fuzz generates 3,392 programs, compares 3,369, and
reports 23 capped programs. It generates 407 TimedWait actions and compares
406: 202/201 in the mostly-well-formed lane and 205/205 in the adversarial
lane. Fixed uncapped probes report both one timeout result and one wake result.

Cross-model inclusion compares 1,747 programs with zero skips and runs 17,470
per-kind checks. Its dedicated TimedWait corpus is 4 attempted / 4 compared /
0 skipped.

The optimality corpora are unchanged. Their complete meter lines remain
byte-identical at SC 1.067, TSO 1.152, and PSO 1.154.

### Build flavors, fresh probes, and like-for-like timing

The exact smoke commands pass 30/30 in both configurations:

- Release `build/`: 28.19 seconds, independently repeated at 23.24 seconds;
- Debug `build-restore/` with `DPOR_ENABLE_RESTORE_ASSERTS=ON`: 310.89
  seconds in the final independent frozen-tree run.

Four verifier-written CLI programs, removed after the run, exercised paths
independent of the compiled fixtures:

- wake-only HB completed cleanly in 5 schedules;
- timeout without a condition edge retained the intended race in 10 schedules;
- a lost notification followed by TimedWait completed cleanly in 1 schedule;
  and
- the canonical timeout/repark loop reported one `fair divergence`, never a
  deadlock.

Performance uses ADR 0029's like-for-like rule. The comparison included 28
like-for-like suites and ran serially to avoid intra-suite contention. It
excluded `cli_tests` because Campaign 16 adds cases inside that pre-existing
binary and excluded Campaign 16's new `timedwait_tests`. Three runs were
interleaved in baseline/candidate order `B,C,C,B,B,C`. Best elapsed time
improved from 21.89 to 20.87 seconds (-4.7%).

As in ADR 0029, this is suite-name like-for-like rather than
corpus-identical. Expanded TimedWait cases inside common suites and the new
gallery entries remain enabled; no added case was removed to improve timing.

The exploration-core gates were also measured individually, baseline versus
candidate, as best-of-three seconds:

| Suite | Baseline | Candidate |
|---|---:|---:|
| `dpor_oracle` | 0.32 | 0.35 |
| `dpor_oracle_3threads` | 3.18 | 3.11 |
| `tso_oracle` | 0.27 | 0.25 |
| `pso_oracle` | 0.15 | 0.14 |
| `dpor_fuzz_differential` | 2.52 | 2.61 |
| `dpor_optimality` | 1.57 | 1.58 |

The small positive deltas are below one tenth of a second while the TimedWait
oracle and fuzz workloads are larger. Neither the common-suite aggregate nor
the per-suite evidence shows a regression beyond measurement noise.

## Decision

Accept TimedWait as an explicit nondeterministic logical-time transition.
Timeout and wake are schedule competitors, not hidden interpreter choices.
Timeout writes `0` and creates no condition edge; Signal/Broadcast wake writes
`1` and retains the existing wake edge. Both paths reacquire the mutex before
source progress.

Keep every same-condition TimedWait/Wait/Signal/Broadcast pair conservative.
Retain exact TimedWait phase/episode and exact Signal/Broadcast wake-target
identity throughout DPOR history. Keep those ordinals out of behavioral
fingerprints, where existing pc, phase, waiter, mutex, and register state are
both necessary and sufficient.

## Consequences

- Lost wakeups remain deadlocks for plain Wait but become explorable timeout
  schedules for TimedWait.
- A parked TimedWait prevents terminal deadlock classification; a blocked
  reacquisition can still be a mutex deadlock.
- Programs can branch deterministically on `0` timeout versus `1` wake.
- Wake retains Signal/Broadcast HB; timeout contributes only a later mutex
  reacquisition edge.
- Time remains logical, nondeterministic, explicit, and replayable.
- Same-condition timeout pairs may overexplore; no unproved independence ships.
- Repeated static endpoints cannot alias different parked episodes or wake
  targets in DPOR history.
- Timeout-spin cycles close under the existing exact behavioral fingerprint
  and tri-state fairness classifier.
- TSO and PSO expose every required drain as an explicit flush.

## Invariants protected

- **Determinism and replay:** every timeout is a numeric schedule choice; no
  wall clock, random branch, or hidden deadline participates.
- **HB soundness:** wake joins exactly the waker clock, while timeout joins no
  condition clock in either direction.
- **DPOR soundness:** conservative dependence and exact dynamic episode/target
  identity prevent unproved class collapse.
- **Deadlock soundness:** a parked timed waiter is enabled; only a blocked
  reacquisition contributes a blocker.
- **Lasso soundness:** absolute episode ordinals stay out of behavioral state,
  while phase and result remain fingerprinted.
- **Weak memory:** TimedWait is an explicit-drain ordered point and timeout
  performs no hidden flush.
