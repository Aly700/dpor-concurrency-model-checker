# ADR 0010: Dynamic Thread Spawn

## Status

Accepted.

## Context

The model checker previously treated every thread body as alive from the
initial state. The IR now needs dynamic thread creation without making the
program itself dynamic: thread bodies remain a static vector, while execution
state records which bodies have been started.

## Decision

Extend `ActionKind` with `Spawn`. `Action::target` is the target thread id for
both `Spawn` and `Join`.

Program bodies stay static. Execution state adds a per-thread `started` bit.
A thread is initially started iff it is not the valid target of any `Spawn`
action in the program. Valid target means in range and not the spawning thread;
self-spawn and out-of-range spawn are invalid actions and do not make the
source thread unreachable at startup, so their modeled error remains
executable through the direct IR.

`Spawn(t)` is enabled whenever the spawning thread is started and not finished.
Executing it ticks the spawner, advances the spawner pc, marks `t` started, and
joins `t`'s vector clock with the spawner's post-tick clock. This is the spawn
happens-before edge: everything before the spawn happens-before every later
action in the spawned thread. The target starts at pc 0. If the target body is
empty, the target becomes finished only after the spawn starts it.

Spawn validation uses two layers:

- The CLI parser rejects out-of-range spawn targets, self-spawn, and duplicate
  spawn targets because the full static program is known after parsing.
- The direct IR reports modeled errors for out-of-range spawn, self-spawn, and
  spawning an already-started target. Duplicate direct-IR spawns are therefore
  detected when any second `Spawn(t)` reaches execution. This keeps generated
  invalid programs replayable through normal modeled-error reports.

`Join(t)` is enabled only when `t` has started and finished. A not-started
thread never satisfies join enabledness, even when its body is empty and its pc
is 0. This explicitly avoids the old pc-only `is_finished` trap.

Termination is aliveness-aware. A terminal state with all started threads
finished is clean even if some static thread bodies were never spawned. A
terminal state with any started unfinished thread is a deadlock. Therefore a
started thread blocked on `Join(t)` deadlocks when `t` is not started and no
enabled action remains that could eventually start it; the deadlock report
records the joiner as waiting on thread `t`. The existing "no enabled action
and not all started threads are finished" rule covers spawners that are
finished or transitively stuck.

## DPOR and Independence

Not-started threads are not schedulable and do not appear in enabled sets.
They also have no enabled transition for sleep sets or normal backtracking.
At terminal leaves, a not-started thread with a non-empty body contributes its
first action as a disabled transition. This lets disabled-transition repair
account for schedules where a pending `Spawn` could have enabled that first
action earlier.

`independent()` treats every pair involving `Spawn` as dependent. This is
deliberately conservative: the action-only predicate cannot see whether the
other transition belongs to the spawn target, and spawn changes both
enabledness and the target vector clock.

Spawn required one DPOR repair refinement. When a later transition from a
spawned thread is disabled at prefixes before the successful `Spawn(t)`, those
prefixes are not useful repair points because `t` did not exist yet. The trace
therefore records which spawn transitions actually started a target, and
disabled-transition repair does not move a target thread's repair before its
successful spawn enabler. Modeled-error spawn attempts are not recorded as
enablers.

Disabled-transition repair is conservative across every dependent disabled
prefix rather than only one earliest prefix. Conservative dependencies such as
`Spawn` and `Join` can be real but too early to make a later transition's own
program-order prerequisites reachable. Adding all enabled threads at every
dependent disabled repair prefix preserves the deadlock and pre-error classes
exercised by spawn/join/condition-variable interactions. Sleep entries at
repair nodes are cleared for those repair choices; otherwise a dynamically
added backtrack can be skipped solely because it was previously slept.

## Replay and Minimization

No replay format change is needed. `Spawn` is a normal schedule step
identified by `(thread, action_index)`. Replaying a target thread before its
spawn is rejected as a disabled action. Schedule minimization needs no special
case: deleting a required spawn makes later target-thread steps invalid or
changes the reproduced bug identity, so replay validation rejects that
candidate naturally.

## Invariants Protected

Happens-before is protected by joining the target clock with the spawner's
post-tick clock on successful spawn. This suppresses races from pre-spawn
writes to spawned-thread accesses while preserving races between post-spawn
spawner accesses and spawned-thread accesses.

Deadlock soundness is protected by making join enabledness started-aware and
by treating unstarted threads as clean only when no started thread is blocked
on them.

DPOR soundness is protected by making `Spawn` dependent with everything,
treating not-started first actions as disabled transitions at terminal leaves,
recording only successful spawn enablers in the trace, and conservatively
repairing every dependent disabled prefix.
