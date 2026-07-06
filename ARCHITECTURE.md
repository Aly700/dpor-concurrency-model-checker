# Architecture — DPOR Model Checker

## Pipeline

```text
controlled concurrent program -> scheduler -> explored schedules -> HB/race/deadlock analysis -> minimal replay
```

## Current scaffold

- `model::Program` is a tiny action IR.
- `ModelChecker::explore_naive` is the exhaustive oracle.
- `ModelChecker::replay` re-executes a deterministic schedule and rejects
  disabled or out-of-range steps with a clear error.
- `VectorClock` is the base happens-before data structure.
- DPOR should be added beside, not instead of, the naive oracle.

## Phase 1 execution model

The checker interprets a program as a small-step state machine:

- `pc[tid]` is the next action for each modeled thread.
- `mutex_owner[mutex]` records the owning thread for held mutexes.
- `mutex_clock[mutex]` records the vector clock stored by the last successful
  unlock of that mutex.
- `thread_clock[tid]` records each thread's happens-before frontier.
- `memory[address]` records the last write and the reads since that write.

At each DFS state, the naive oracle enumerates exactly the enabled actions in
ascending thread-id order. `Read`, `Write`, `Yield`, and `Unlock` are enabled;
an invalid `Unlock` is reported as a modeled error. `Lock` is enabled only when
its mutex is not currently owned. A state with unfinished threads and no enabled
action is a deadlock; a state where all threads are finished is normal
termination.

## Happens-Before Analysis

Each executed step ticks its thread's vector clock. `Unlock` stores the
releasing thread's clock in the mutex clock. `Lock` joins the acquiring thread's
clock with that mutex clock. Memory accesses compare their current vector clock
against prior conflicting accesses to the same address. A race report records
the two access endpoints and the executed prefix through the second access.

The exhaustive oracle explores all enabled interleavings of the modeled
small-step semantics. Therefore, per-execution happens-before race detection
over every explored schedule is complete for this model: if a modeled race or
deadlock is reachable in any enabled interleaving, the naive oracle will visit a
prefix that reports it.

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
