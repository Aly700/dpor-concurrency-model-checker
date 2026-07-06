# ADR 0005: Join and Mesa Condition Variables

## Status

Accepted.

## Context

The IR now needs thread joins and condition variables without weakening the
naive oracle. `INVARIANTS.md` still requires deterministic replay as a sequence
of `(thread, action_index)` steps, and DPOR may prune only through a documented
independence predicate.

## Decision

Extend `ActionKind` with `Join`, `Wait`, `Signal`, and `Broadcast`.
`Action::target` is the target thread id for `Join`. `Action::condition` names
the condition variable, and `Action::mutex` names the mutex used by `Wait`.

`Join(target)` is enabled only after `target` has finished all of its actions.
Out-of-range joins and self-joins are executable modeled errors. A successful
join adds a happens-before edge by joining the caller's vector clock with the
target thread's final clock.

`Wait(cv, mutex)` is encoded as two schedule steps with the same
`(thread, action_index)`:

- First step: the thread must own `mutex`, or the step reports a modeled error.
  It ticks, performs the same release clock update as `Unlock`, releases the
  mutex, enters `cv`'s ordered waiter set, and leaves `pc` on the `Wait`.
- Second step: after a signal or broadcast wakes the thread, the same schedule
  pair is enabled only when `mutex` is free. It ticks, performs the same acquire
  join as `Lock`, clears the wait phase, and advances `pc` past the `Wait`.

The phase is stored in per-thread interpreter state. Programs are not rewritten.
Replay validates the phase implicitly through enabledness: a sleeping waiter is
disabled, and a woken waiter can replay only the original action index when the
mutex reacquire is enabled.

`Signal(cv)` wakes the lowest-numbered waiting thread. `Broadcast(cv)` wakes
all current waiters in ascending thread-id order. Both operations are no-ops
when no thread is waiting; they do not queue permits. This models Mesa
condition variables and makes lost-wakeup deadlocks real. A signal or broadcast
adds a happens-before edge to every thread it wakes by joining the woken
thread's clock with the signaler's clock at wake time. The later mutex
reacquire adds the usual acquire edge too.

Deadlock reporting now tags each unfinished disabled thread as waiting on a
mutex, a thread, or a condition variable. This preserves the replay invariant:
the minimized deadlock identity records the actual terminal blockers visible to
replay.

## Independence Clauses

The predicate remains conservative:

- Conflicting same-address memory operations are dependent because swapping can
  change memory metadata, race endpoints, and observed state.
- Any pair involving `Join` is dependent in the public action-only predicate.
  A tighter rule would require the owning thread of the other action; marking
  all joins dependent safely covers the required `Join(t)` dependency with
  every action of thread `t`.
- Any two operations on the same condition variable are dependent because Wait,
  Signal, and Broadcast mutate the same waiter/woken state and can change
  future enabledness.
- Wait is dependent with operations on its mutex. More generally, Lock, Unlock,
  and Wait on the same mutex are dependent because ownership, release/acquire
  clocks, and enabledness can change with order.
- Signal or Broadcast on different condition variables are independent when no
  other clause applies. They mutate disjoint waiter sets, do not queue permits,
  and leave the same enabledness effects in both orders.
- Remaining pairs are independent only when they touch disjoint memory, mutex,
  condition, and join state and therefore commute to the same modeled state.

## Invariants Protected

Soundness is protected by making new uncertain pairs dependent and by keeping
the naive oracle as the reference semantics. DPOR still cross-validates verdict
equality, schedule dominance, and replay identity against the naive oracle over
widened Join/Wait/Signal/Broadcast alphabets.

Replay is protected by the two-step Wait encoding: schedules remain plain
`(thread, action_index)` pairs, and replay reaches the same release/sleep or
reacquire/resume phase from interpreter state.

Happens-before is protected by four explicit synchronization edges: unlock-style
release during Wait, signal/broadcast-to-woken waiter, lock-style reacquire
after wake, and target-final-clock-to-joiner during Join.

Deadlock detection is protected by treating blocked joins, sleeping condition
waiters, and woken waiters blocked on mutex reacquire as terminal disabled
transitions. DPOR applies the same disabled-transition fallback to these next
actions that it already applied to blocked locks.
