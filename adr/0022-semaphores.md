# ADR 0022: Counting semaphores with accumulated release clocks

## Status

Accepted.

## Context

The checker already has mutexes, condition variables, reader-writer locks,
atomics, and thread lifecycle synchronization. Counting semaphores add a
different enabledness shape: a post queues a permit even when no waiter exists,
and each successful wait consumes one permit. This is deliberately unlike a
condition-variable signal, which is lost when there is no current waiter.

The hard design question is happens-before. A real execution can conceptually
match a successful wait to one earlier post whose permit it consumes. Permits
in this IR are anonymous, however. Exploring an exact post-to-wait match would
add a second nondeterministic choice beside thread scheduling, and that choice
would have to become part of replay, DPOR dependence, and report identity.

Race detectors use similarly object-level synchronization abstractions. LLVM
ThreadSanitizer exposes acquire/release annotations keyed by one synchronization
address, and Helgrind documents that a semaphore wait acquires from a posting
thread while the particular post is unspecified when there were multiple
posts. Those are analogies, not claims that either tool implements this ADR's
exact clock equation: [LLVM TSan interface](https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/tsan_interface.h#L22-L25),
[Valgrind Helgrind manual](https://valgrind.org/docs/manual/hg-manual.html#hg-manual.hb).

## Decision

### Actions, namespace, and permit semantics

The IR adds `SemPost(name)` and `SemWait(name)`. Their strict text spellings
are `sem_post name` and `sem_wait name`.

Every semaphore begins with zero permits. There is no declaration or
initialization action; source programs seed permits with explicit `sem_post`
actions. `SemPost` is always semaphore-enabled, increments the count by one,
and has no modeled ceiling or error endpoint. `SemWait` is enabled exactly
when the count is positive. A successful wait decrements the count by one;
at zero it remains disabled.

Semaphore names form a resource namespace distinct from both mutex names and
reader-writer-lock names. A program using one name in either semaphore action
and also in `Lock`, `Unlock`, the mutex operand of `Wait`, or any rwlock action
is rejected at parse/load time. Addresses and condition-variable names remain
separate domains and may reuse the spelling.

The behavioral state contains the canonical map of nonzero semaphore permit
counts. Zero-count entries are equivalent to absence. This is required for
lasso soundness: a loop that only posts grows behaviorally and must not be cut
as a repeated state, while a post/wait loop that returns the count to zero may
repeat even though analysis clocks have advanced.

Under TSO and PSO both semaphore actions are ordered synchronization points.
They remain disabled until every store buffer belonging to the executing
thread is empty. They never perform a hidden flush.

### Strong accumulated happens-before model

Each semaphore stores a lifetime-monotone accumulated release clock `R` in
addition to its permit count. After the common per-step tick, successful
operations update clocks as follows:

```text
SemPost(s) by thread t:
    permits[s] := permits[s] + 1
    R[s] := R[s] join C[t]

SemWait(s) by thread t, when permits[s] > 0:
    permits[s] := permits[s] - 1
    C[t] := C[t] join R[s]
```

`SemPost` is release-only: it never acquires the old accumulator into the
poster. `SemWait` never clears or replaces the accumulator. Consequently two
posters do not gain a poster-to-poster edge, while every later successful wait
acquires every post release accumulated so far.

This is intentionally a **strong semaphore model**. A wait can become ordered
after a post whose permit it did not consume. Exact permit-to-wait matching
would require exploring and replaying that matching nondeterminism; this IR
does not do so. The checker therefore verifies programs against this strong
model, not against every possible concrete permit lineage. The strength can
hide a race that would exist under a weaker exact-matching semantics, so this
caveat is part of the public model rather than an implementation detail.

The accumulated clock is HB analysis instrumentation and is excluded from
behavioral-state fingerprints. The permit count is included because it changes
enabledness.

### DPOR independence

Every same-name pair involving `SemWait` is dependent. A wait consumes shared
count, can disable another wait, acquires the current accumulator, and can
observe a different HB frontier depending on which posts precede it.

Two cross-thread, co-enabled `SemPost(s)` transitions are independent. Let the
initial count and accumulator be `n` and `R`, and let the posters' post-tick
clocks be `Ci` and `Cj`. Either adjacent order leaves:

```text
permits = n + 2
R = R join Ci join Cj
```

Vector-clock join is associative and commutative. Neither poster acquires the
other, so both thread clocks, PCs, registers, memory/race state, buffers, and
all other resources are also identical. Each post remains enabled after the
other. When `n` is zero, either first post enables the same set of blocked
wait endpoints, and both final enabled sets are identical.

The middle traces are not collapsed:

```text
Post(i); Wait; Post(j)
Post(j); Wait; Post(i)
```

The wait can acquire different one-post frontiers in those traces. Because
`Post/Wait` stays dependent, persistent-set closure retains the wait and the
remaining post after either first post. At an earlier zero-permit prefix the
wait uses the existing all-enabled disabled-transition repair. A repair that
selected only the post observed in one execution would be unsound, especially
when an alternate poster first needs a source prerequisite or buffered flush.
The conservative fallback is therefore intentional.

Operations on different semaphore names are independent, subject to existing
same-thread, Spawn, Join, memory-model, and terminal safeguards.

The exact focused discriminator is:

```text
T0: SemPost(s); SemWait(s)
T1: SemPost(s)
```

It has three naive leaves and two DPOR representatives. With all same-name
semaphore operations conservatively dependent, DPOR would retain all three.
The three-thread `Post | Post | Wait` shape has four legal leaves and three
semantic trace classes, but the current backtrack-set algorithm explores all
four because of the slept-transition repair redundancy described in ADR 0020.
No wakeup-tree optimization is introduced here.

### Deadlock, replay, and verification

A terminal zero-permit `SemWait` is tagged `BlockedOnKind::Semaphore` with the
semaphore name. CLI reports render it as `semaphore name waiting_for_post`.
The blocker kind and name participate in deadlock identity, minimization, and
byte-identical replay.

Focused probes pin:

- zero initialization, explicit seeding, one-permit decrement, and exhaustion;
- post publication to waiter acquisition;
- componentwise accumulation across multiple posters and persistence across
  multiple waits;
- absence of a poster-to-poster acquire edge;
- a mutation in which dropping the wait-side join flips the publication probe
  from clean to a race;
- the post/post commuting relation and the alternate-poster middle-wait race;
- TSO/PSO drain discipline for both actions;
- permit-count inclusion and release-clock exclusion in lasso fingerprints;
- parser, namespace, blocker, report, and replay identity.

The two-thread oracle, sampled three-thread oracle, deterministic differential
fuzz generator, and SC-to-TSO-to-PSO model-inclusion corpus all include both
actions. Focused gate results are: 22,126 two-thread programs with 60,791 naive
versus 34,481 DPOR schedules; 65,544 sampled three-thread programs with 845,471
versus 362,789; 3,000 fuzz programs with 1,002,798 versus 87,775 (32 capped
programs excluded from verdict equality); and 1,711 complete cross-model
programs performing 17,110 inclusion checks with zero skips. Every extended
gate retained verdict equality and schedule dominance. The committed
optimality corpus is unchanged, preserving the meter's SC 1.067, TSO 1.152,
and PSO 1.154 baselines.

## Consequences

- Semaphore programs can queue anonymous permits and express publication
  without introducing a declaration action.
- The strong accumulator is deterministic, compact, and replay-neutral, but it
  can add HB edges beyond an exact permit matching. Users must interpret safe
  verdicts under that documented model.
- Same-name post/post schedules receive a proved reduction while all wait
  interactions remain conservative.
- Permit counts enlarge behavioral state. Accumulated clocks enlarge analysis
  state only and do not prevent valid lasso closure.
- Buffered models gain no hidden behavior: semaphore operations merely wait for
  the existing explicit flush transitions.

## Invariants protected

- **Happens-before:** posts only release; waits acquire the lifetime accumulator;
  posters do not acquire one another; the strength caveat is explicit.
- **Independence soundness:** only the direct post/post diamond is widened, and
  the waiter middle witnesses retain conservative disabled repair.
- **Replay:** semaphore actions use ordinary numeric source steps, while blocker
  kind/name and strong-HB execution are deterministic across check and replay.
- **Deadlock soundness:** every unfinished zero-permit waiter is reported as a
  semaphore blocker, never clean termination or a condition-variable wait.
- **Lasso soundness:** counts are behavioral and accumulated clocks are analysis
  instrumentation.
- **Namespace validation:** one name cannot acquire incompatible mutex, rwlock,
  and semaphore ownership/HB semantics.
