# Invariants — DPOR Model Checker

## Soundness

- The checker may explore too many schedules, but must not skip a schedule class that can expose a distinct bug.
- Independence may only be claimed for actions whose ordering cannot affect future enabledness or observable state.
- A report of safe means no explored state and no pruned equivalent class contains a bug under the modeled semantics.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes.
- Replaying a schedule must produce the same state, report, and trace.
- Test programs must not depend on OS thread scheduling.

## Happens-before

- Release/acquire edges must update vector clocks consistently.
- Conflicting memory accesses unordered by happens-before are races.
- Deadlock detection must distinguish a true cycle from a voluntarily finished thread.
