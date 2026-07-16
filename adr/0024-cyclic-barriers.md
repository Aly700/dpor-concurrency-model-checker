# ADR 0024: Cyclic barriers with generation-exact HB and last-arrival dependence

## Status

Accepted.

## Context

The checker already models blocking ownership, permit, condition-variable, and
thread-lifecycle synchronization. A cyclic barrier has a different shape: an
arrival is a real transition, but a non-last participant remains at the same
source action and becomes disabled; one later arrival releases several parked
threads at once; and the same source endpoint can participate again after the
barrier resets.

The happens-before requirement is collective rather than pairwise. Every
released participant must acquire every arrival in exactly its generation.
Retaining an old generation's accumulator would fabricate edges and hide real
races, while joining only the last arrival would omit publication edges and
report false races.

The reduction question is state-dependent. Two early arrivals can commute, but
the transition that fills the party changes enabledness and releases other
threads. An action-only independence predicate cannot know the current arrival
count or distinguish repeated occurrences of the same cyclic source endpoint.
The optimization is accepted only with corresponding persistent-set,
sleep-set, disabled-transition, and occurrence-identity safeguards.

## Decision

### Action, validation, namespace, and replay

The IR adds `BarrierWait(name, parties)`. Its strict text spelling is
`barrier_wait NAME PARTIES`. `parties` is an unsigned positive count, and every
action with the same barrier name must specify one program-wide count. The
strict CLI validates both rules while loading: zero, malformed or overflowing
counts, wrong arity, and disagreement with the first use are parse errors.

The direct C++ `Program` API intentionally follows the existing forward-error
discipline. It canonically records the first program-order count for each name;
an action with zero parties or a different count remains executable and
produces a deterministic modeled error at that action's endpoint. This keeps
directly constructed malformed programs replayable instead of turning them
into constructor failures.

A barrier name is a synchronization-resource namespace distinct from mutexes
(including the mutex operand of `Wait`), reader-writer locks, semaphores, and
condition variables. Both the parser and direct `ModelChecker` construction
reject collisions in either textual order. Address and register names remain
separate domains.

Under TSO and PSO, `BarrierWait` is a full ordered point. It is disabled until
every pending store buffer belonging to the arriving thread has drained, and it
performs no hidden flush. SC behavior is unchanged.

One arrival is one ordinary numeric schedule step. A valid participant is
inserted into the current generation's canonical thread-id set and cannot be
scheduled at that endpoint again while parked. If the set remains smaller than
`parties`, its pc stays on the `BarrierWait`. The last arrival atomically
advances every participant past its own wait, clears the generation state, and
increments a private generation ordinal. There is no separate release step and
no phase suffix in the public schedule format. `parties == 1` takes this same
path immediately.

An execution with unfinished participants parked in an incomplete generation
is a deadlock. Each participant carries `BlockedOnKind::Barrier` and the
barrier name; CLI output renders `barrier NAME waiting_on_barrier`. Blocker
identity, minimization, check output, and replay output remain deterministic and
byte-identical.

### Generation-exact happens-before

Let `A_g` be the participants in generation `g`, and let `C_i^arr` be
participant `i`'s clock after the common transition tick on its arrival. The
barrier holds an accumulator `R_g`, initially empty, and applies:

```text
on each arrival i in generation g:
    R_g := R_g join C_i^arr

when |A_g| == parties:
    for every i in A_g:
        C_i := C_i join R_g
        pc_i := pc_i + 1, normalized past labels
    A_g := empty
    R_g := empty
    generation := generation + 1
```

Thus every released thread acquires the componentwise join of all arrivals in
that generation, including the last arrival, before any participant continues.
The parked-to-last and last-to-parked directions are both required.

Clearing `A_g` and `R_g` is semantically essential. A disjoint participant set
in generation `g + 1` does not acquire generation `g` merely because it reused
the barrier name, and participants from `g` are not released a second time.
Cross-generation HB still arises when a real participant carries its joined
thread clock through program order into a later arrival; clearing the barrier
does not erase that thread-local chain.

Fixed verdict-flip probes cover all of these edges. The two positive probes
place a plain publication before either the parked or last arrival and require
the opposite participant's post-release read to remain clean. Two negative
probes use disjoint consecutive generations in opposite directions and require
the real race to remain visible, catching a leaked accumulator or retained old
participant. A final positive probe requires a shared participant's actual
program-order chain across consecutive generations to remain clean. Every
probe checks naive/DPOR agreement and exact witness replay.

### Behavioral state and ADR 0023's classifier

For each nonempty barrier, the exact behavioral fingerprint includes the name,
configured party count, and sorted current-generation arrival set. Empty
barriers have the canonical absence representation. This state affects both
enabledness and deadlock, so omitting it could turn an undersubscribed wait into
a false lasso.

The accumulated arrival clock and absolute generation ordinal are excluded.
They are HB/occurrence instrumentation and cannot be read by a modeled program.
Including the ordinal would prevent a genuinely repeating balanced barrier loop
from closing its lasso; including the clock would likewise confuse advancing
analysis state with changing behavior.

This new same-pc action extends ADR 0023's fingerprint-elision proof. In a
program without a normalized self/backward branch, a non-last arrival strictly
grows the fingerprinted set and disables its participant. That set can return
to empty only when another thread executes a source arrival that advances every
parked pc. None can then return to its old barrier pc without backward control
flow. A one-party wait advances immediately; an invalid wait advances to a
terminal modeled error; buffered flushes retain their existing finite-drain
measure. Therefore barrier programs without backward control flow remain safe
to classify as acyclic and may elide fingerprint construction. Focused metrics
tests cover completed and undersubscribed acyclic barriers, a blocked backward
loop with no false cycle, and a balanced cyclic-barrier lasso with exact history
restore.

### State-dependent DPOR independence

The public `independent(Action, Action)` relation keeps every same-name barrier
pair dependent because it has no node state. The checker-local refinement is
limited to two different threads whose valid `BarrierWait` endpoints are both
enabled, name the same barrier, carry the same generation stamp as the node,
and have not already arrived.

Let `k` be the node's current arrival count and `p` its configured parties.
Those two arrivals are independent exactly when:

```text
k + 2 < p
```

The strict inequality means neither adjacent order releases the generation.
Both orders tick the same two disjoint thread clocks, leave both pcs parked,
insert the same two thread IDs, and produce the same accumulator
`R join C_i join C_j`. Vector-clock join and set insertion are commutative, so
registers, memory values, ownership, buffers, race metadata, all other
resources, and the resulting enabled set are identical.

When `k + 2 == p`, the second arrival completes the party and runs the
collective release. It is dependent: the identity of the last transition and
the intermediate enabled set differ, and that release has HB/enabledness
effects on every parked participant's later action. Invalid counts,
cross-generation occurrences, already-arrived participants, and any pair not
found as exact enabled transitions in the node remain dependent. Arrivals on
different names commute subject to the unchanged same-thread, Spawn, Join,
memory, buffered-transition, and terminal safeguards.

### Persistent, sleep, disabled, and occurrence safeguards

The direct two-transition diamond does not by itself prove the reduction
sound. Four additional rules are part of this decision:

1. Initial persistent-set closure inserts every co-enabled valid same-name,
   same-generation arrival sibling, even when a pair passes the early-arrival
   diamond. This retains executions where another thread runs between the
   arrivals and changes which participants belong to or complete the
   generation. The four-thread publication discriminator depends on this rule:
   an alternate three-thread cohort can release without the publisher and must
   retain its race witness.
2. Sleep inheritance evaluates the relation against the parent/pre-transition
   barrier snapshot, not the child's incremented arrival count. It inherits an
   entry only if the same effective endpoint and generation-stamped occurrence
   remains enabled in the child. Using the post-state count would misclassify
   the second early arrival in a three-party generation.
3. A last arrival is explicitly dependent with the later action of every
   parked participant it releases. An incomplete barrier has no unique enabler,
   so disabled-transition repair uses the conservative all-enabled fallback.
   The fallback is applied through the ordinary last-point path, terminal-leaf
   repair, and sleep-blocked repair, preserving the loop 10/11 safeguards.
4. Enabled transitions, executed trace entries, and DPOR node snapshots retain
   the private barrier generation. This prevents a cyclic execution from
   equating two occurrences that share the public `(thread, action_index)` but
   belong to different generations.

The exact three-thread discriminator is:

```text
T0: BarrierWait(b, 3)
T1: BarrierWait(b, 3)
T2: BarrierWait(b, 3)
```

Naive exploration visits all `3! = 6` arrival orders. For each fixed last
arriver, the other two are early at `k == 0`, satisfy `0 + 2 < 3`, and commute;
their two orders collapse to one representative. The last arrival is retained
as dependent, and there are three possible last arrivers, so DPOR explores
exactly 3 schedules. This is one class per release transition, not an attempt to
collapse the collective release itself.

### Gallery, parser, and verification gates

The classic gallery adds a three-worker phased computation that reuses one
three-party barrier for two generations. The correct program is clean. Its
paired broken program omits worker 2's final arrival, so workers 0 and 1
deadlock with barrier blockers. Direct naive, DPOR, CLI check, and replay
witness payloads are pinned to identical bytes after excluding only
exploration-scope counters that a single-schedule replay cannot reproduce.

All differential alphabets are widened with fixed party counts per generated
name. The acceptance outputs are:

- two-thread oracle: 22,269 programs, alphabet 24, 79,060 naive versus 35,882
  DPOR schedules;
- three-thread oracle: 65,544 programs, alphabet 22, 833,863 naive versus
  336,551 DPOR schedules;
- TSO oracle: 10,698 programs, alphabet 12, 121,409 naive versus 28,970 DPOR
  schedules;
- PSO oracle: 5,579 programs, zero capped skips, alphabet 12, 54,521 naive
  versus 15,920 DPOR schedules;
- fixed 3,000-program differential fuzz run: 1,100 generated barrier waits,
  2,977 programs compared, and 23 capped programs reported rather than used for
  verdict equality;
- cross-model inclusion: 1,717 complete programs and 17,170 per-kind checks,
  including all 4 of 4 dedicated barrier programs with zero barrier skips.

Every oracle retains naive/DPOR verdict equality, DPOR schedule dominance, and
report replay identity. The optimality corpora intentionally contain no new
barrier programs; exhaustive action rendering/key checks are widened, while the
meter lines remain byte-identical at SC 1.067, TSO 1.152, and PSO 1.154.

## Consequences

- Correct phased programs gain a deterministic cyclic synchronization
  primitive with exact all-arrivals publication in both directions.
- Barrier execution stores one sorted participant set, one clock accumulator,
  and one private ordinal per name. Release can advance several thread pcs in a
  single scheduled transition.
- Generation reset avoids false HB edges, while real cross-generation chains
  remain carried by participant clocks.
- Same-name DPOR pruning is deliberately narrow and stateful. Its machinery
  costs node barrier snapshots and generation stamps, but preserves three
  proved representatives in the three-party discriminator instead of falling
  back to all six schedules.
- Fingerprint elision remains available to acyclic barrier programs because the
  ADR 0023 well-foundedness proof is explicitly extended.

## Invariants protected

- **Happens-before:** every participant joins exactly all arrival clocks from
  its generation; reset creates no name-only edge to a later generation.
- **Independence soundness:** only non-releasing early pairs commute, with
  persistent, parent-snapshot sleep, disabled-repair, last-release, and
  generation-identity safeguards treated as part of the proof.
- **Replay:** each arrival has one numeric endpoint; collective advancement,
  malformed direct-API errors, and blocker identity reproduce deterministically;
  generation stamps keep DPOR occurrence identity separate across cyclic reuse.
- **Deadlock soundness:** an incomplete generation reports every parked
  participant as `waiting_on_barrier`, never clean completion or a generic
  condition wait.
- **Lasso soundness:** active arrivals are behavioral; HB accumulators and
  absolute generation ordinals are analysis state and do not obstruct a true
  cyclic repeat.
- **Namespace and buffered ordering:** one name cannot acquire incompatible
  barrier/mutex/rwlock/semaphore/condition semantics, and TSO/PSO arrivals wait
  for explicit buffer drains without hidden flushes.
