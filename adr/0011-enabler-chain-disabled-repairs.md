# ADR 0011: Enabler-Chain Disabled Repairs

## Status

Accepted.

## Context

ADR 0010 made disabled-transition repair conservative across every dependent
disabled prefix. That fixed real misses involving spawn, join, sleep sets, and
condition variables, but it also made blocked-heavy fuzz programs explore far
too many schedules: on the committed fuzz seeds, DPOR explored 505,276 schedules
versus 1,047,152 for naive.

The goal is to recover exploration efficiency without weakening the
`INVARIANTS.md` soundness rule: DPOR may explore extra schedules, but it must
not skip a schedule class that can expose a distinct modeled race, deadlock, or
modeled error.

## Options Considered

1. Sharper disabled repairs. Compute the concrete enabler chain for a disabled
   transition at a prefix and add only enabled threads that can advance that
   chain. Fall back to ADR 0010's all-enabled repair whenever the chain is not
   computable.
2. Source-DPOR with sleep sets. Replace backtrack sets with source sets. This
   would be a larger rewrite of `dpor_dfs`.
3. Source-DPOR plus wakeup trees. This targets optimality for sleep-set-blocked
   executions, but it is the largest algorithmic change.

## Decision

Implement option 1, with one local enabled-transition refinement for `Join`.
The checker remains a Flanagan-Godefroid backtrack-set DPOR with sleep sets; it
does not switch to Source-DPOR.

Each DPOR node now stores a small enabledness snapshot: `pc`, `started`,
`mutex_owner`, and `wait_phase`. When a later dependent transition is disabled
at a repair prefix, DPOR tries to compute first enabled heads of that
transition's enabler chain:

- If the transition's thread is not started, the chain is a remaining valid
  `Spawn(thread)`; if the spawner itself must first advance, recurse to the
  spawner's chain.
- If the transition is a disabled `Join(target)`, the chain is the target's
  remaining execution until started and finished.
- If a chain thread is enabled, that thread is the first head to add.
- If a chain thread is blocked on a mutex, recurse to the current mutex owner.
- If a chain thread is sleeping in `Wait(cv, m)`, recurse to a remaining
  `Signal(cv)` or `Broadcast(cv)` thread.
- If the chain is cyclic, absent, invalid, or otherwise not proven, use the
  previous all-enabled fallback.

Top-level blocked `Lock` and woken-`Wait` reacquire repairs deliberately keep
the all-enabled fallback. The existing m/n deadlock counterexample shows that
owner-only repair for a blocked mutex acquisition can miss a deadlock where an
unrelated enabled thread must acquire another mutex before the owner releases.

Sleep-set repair is also narrowed: when a computed repair inserts specific
threads, only those inserted threads are removed from the node sleep set. The
all-enabled fallback still removes all enabled repair choices from sleep. This
preserves the ADR 0010 invariant that a dynamically added repair cannot be
skipped solely because it was previously asleep.

Finally, DPOR uses a checker-local transition independence refinement for
enabled valid `Join(target)`. The public action-only `independent()` predicate
remains conservative. Inside DPOR, an enabled valid join is independent of an
unrelated non-`Spawn` transition when the other thread is not the join target
and the other action is not a valid `Join` waiting for the joiner to finish.
Invalid joins, target-thread transitions, spawns, and join cycles remain
dependent.

## Soundness Argument

For not-started threads, no omitted enabled thread can make the target's next
transition executable unless it advances a remaining `Spawn(target)` chain.
Threads not on that chain leave `started[target]` false, so adding them at this
repair point cannot produce the missing target-before-dependent ordering. If no
such chain is proven, DPOR falls back to all enabled threads.

For `Join(target)`, enabledness depends only on `target` being started and
finished. A repair head that advances the target, a spawn of the target, or a
blocking owner/signaler needed by the target can change that enabledness. An
omitted thread that is not on this chain cannot finish the target or make the
join executable; distinct bugs involving the omitted thread are still handled
by normal DPOR when that thread's own dependent transition is executed or
observed as disabled. Cycles and unknown blockers fall back to all enabled
threads.

For sleeping `Wait(cv, m)`, only a `Signal(cv)` or `Broadcast(cv)` chain can
wake the waiter. Omitted threads on other condition variables or unrelated
state cannot change this waiter's enabledness. If no wake chain is visible,
the repair is not narrowed.

For mutex acquisition, the narrowing is intentionally not applied as a
top-level disabled repair. Although the owner is the immediate enabler, an
unrelated enabled thread can still be required to expose a later lock-order
deadlock. Keeping the all-enabled fallback preserves the ADR 0003/0010
deadlock argument.

For enabled valid `Join(target)`, the target is already finished. Executing the
join only joins the target's final clock into the joiner and advances the
joiner. Reordering it with an unrelated non-spawn transition does not mutate
shared modeled state, does not change that transition's enabledness, and does
not create or remove a happens-before edge involving the unrelated thread.
The refinement is not used for invalid joins, transitions by the target,
spawns, or joins that wait for the joiner; those are the cases where enabledness
or error termination can change.

All repair and dependency sets remain sorted vectors or maps, so exploration
order stays deterministic.

## Tests Added

`tests/dpor_oracle.cpp` now contains two blocked-heavy upper-bound tests:

- one joiner blocked on a three-step target with two unrelated yield-only
  workers, bounded at 4 schedules;
- two joiners blocked on the same three-step target with one unrelated
  yield-only worker, bounded at 4 schedules.

The comments document the hand-counted semantic chain: the target's three
`Yield` actions are the only enabler chain before the joins; unrelated
yield-only workers cannot enable the join or change any modeled verdict.

## Measurements

Before:

```text
dpor_oracle: programs checked=21390 alphabet=17 cap_per_length_pair=2048 naive schedules total=64287 dpor schedules total=37513
3-thread sweep: programs=65542 alphabet=15 action_slots=6 cap=65536 naive_schedules=921765 dpor_schedules=586886 strict_reductions=25052
fuzz_differential: programs=3000 checked=2970 skipped_capped=30 races=158 deadlocks=492 errors=411 naive_schedules=1047152 dpor_schedules=505276
```

After:

```text
dpor_oracle: programs checked=21390 alphabet=17 cap_per_length_pair=2048 naive schedules total=64287 dpor schedules total=37503
3-thread sweep: programs=65542 alphabet=15 action_slots=6 cap=65536 naive_schedules=921765 dpor_schedules=477916 strict_reductions=30167
fuzz_differential: programs=3000 checked=2970 skipped_capped=30 races=158 deadlocks=492 errors=411 naive_schedules=1047152 dpor_schedules=108897
```

Additional fuzz seeds:

```text
seed 1000001: fuzz_differential: programs=750 checked=742 skipped_capped=8 races=44 deadlocks=116 errors=99 naive_schedules=270388 dpor_schedules=84891
seed 1000002: fuzz_differential: programs=750 checked=742 skipped_capped=8 races=41 deadlocks=131 errors=106 naive_schedules=314179 dpor_schedules=40449
seed 1000003: fuzz_differential: programs=750 checked=741 skipped_capped=9 races=43 deadlocks=111 errors=104 naive_schedules=267092 dpor_schedules=58514
seed 1000004: fuzz_differential: programs=750 checked=739 skipped_capped=11 races=45 deadlocks=122 errors=103 naive_schedules=278029 dpor_schedules=30714
seed 1000005: fuzz_differential: programs=750 checked=747 skipped_capped=3 races=44 deadlocks=135 errors=97 naive_schedules=155194 dpor_schedules=12698
```
