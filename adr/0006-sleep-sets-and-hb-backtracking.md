# ADR 0006: Sleep Sets and Happens-Before Backtracking

## Status

Accepted.

## Context

ADR 0003 intentionally implemented a conservative DPOR variant: every dependent
executed pair could add a backtrack at every earlier dependent prefix, and no
sleep sets were used. That was sound but explored many redundant schedules,
especially when independent preludes led to the same later dependent action.

The naive oracle remains the semantic reference. DPOR may reduce only by the
documented `independent()` predicate, by happens-before ordering that cannot be
reversed, and by classic sleep-set pruning of already represented trace classes.

## Decision

`explore_dpor` now records each executed transition with:

- the replay endpoint `(thread, action_index)`;
- a phase-aware effective action used only for dependence checks;
- the executing thread's vector clock immediately after the step.

Dynamic backtracking now scans the current trace backward and adds at most one
normal backtrack: the last earlier transition that is dependent with the current
effective action and is not ordered before it by vector-clock happens-before.
Earlier required reversal points are added inductively when those prefixes are
reached.

When the later effective transition was not actually enabled at a candidate
prefix, DPOR keeps the conservative disabled-transition repair: it adds every
enabled thread at an earlier dependent prefix. The node records the enabled
endpoint and effective action for each enabled thread, so this check is
phase-aware and does not confuse a thread enabled at an earlier action with the
later transition itself.

Each node also carries a sorted sleep set. A child inherits the parent's sleep
set minus entries whose phase-aware next action is dependent with the transition
just executed; entries that are no longer enabled are dropped. A transition whose
thread is asleep is never executed. After exploring a thread from a node, that
thread is added to the node's sleep set for subsequent alternatives.

## Soundness Arguments

HB-aware backtracking is sound for an enabled transition pair because an
HB-ordered pair cannot be reversed by any schedule in the same reachable trace
class. Reversing it would violate a release/acquire, wake, join, or same-thread
edge already represented in the vector clocks. Therefore skipping that
backtrack cannot remove the only representative of a race, deadlock, or modeled
error class.

The disabled-transition fallback is intentionally more conservative. If the
later effective transition is not enabled at the prefix, an HB edge observed in
the current trace may depend on prerequisites that would run in a different
place after fallback. Adding all enabled threads at the repair point preserves
deadlocks and pre-error bugs whose witness requires reaching that later
transition before the earlier dependent action.

Sleep sets are sound because a sleep-blocked execution is Mazurkiewicz-equivalent
to one already explored from the same prefix context. Since the checker keeps at
least one representative of each unblocked trace class, and per-execution
happens-before race detection is invariant within a class, race/deadlock/error
existence verdicts are preserved. The first-found report may change; replay and
minimization remain the public identity contract.

## Wait, Signal, Broadcast, and Join Landmines

`Wait(cv, mutex)` is one IR action but two effective transitions. The
release/sleep phase remains an effective `Wait` because it mutates the condition
wait set and releases the mutex. The woken reacquire phase is reduced as an
effective `Lock(mutex)`. This prevents sleep sets and backtracking from treating
the reacquire as dependent with unrelated later `Signal` or `Broadcast` actions
on the same condition variable.

`Signal` and `Broadcast` can change other threads' enabledness. The wake edge is
joined into each woken thread before a later reacquire transition is recorded,
so HB-aware skipping against the concrete waker is valid. Different waiter-set
orders are still represented because `Wait`, `Signal`, and `Broadcast` on the
same condition variable remain dependent before the wake edge exists.

`Join(target)` is enabled only after the target has finished. The public
`independent()` predicate conservatively makes every Join dependent, and
HB-aware skipping applies on top: after a successful join, the joiner's clock
includes the target's final clock, so target actions ordered before the join do
not need reversal backtracks.

Modeled errors still terminate a schedule. At an error endpoint DPOR clears the
current node's sleep set and adds every enabled sibling to backtrack, preserving
the previous no-pruning behavior for bugs reachable before the error. A fully
sleep-blocked prefix still applies disabled-transition fallback before it is
pruned, so blocked transitions visible before a slept error endpoint can repair
earlier prefixes.

## Determinism

All sets remain sorted vectors or `std::map`. Exploration choices are still made
in ascending thread id, and sleep-pruned prefixes do not increment
`max_schedules`; the cutoff counts only executed representative schedules.
