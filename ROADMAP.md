# Roadmap — Stateless Concurrency Model Checker with DPOR

1. Cooperative scheduler and deterministic replay.
2. Naive exhaustive interleaving enumeration on tiny programs.
3. Vector-clock happens-before tracking.
4. DPOR pruning with sound independence predicates.
5. Deadlock/race reports with minimal reproducing schedules.

## Phase 1 first tasks

- Keep naive exhaustive scheduling as the reference oracle.
- Add vector-clock happens-before tracking to each synchronization event.
- Implement independence predicates with explicit comments for soundness.
- Add minimal reproducing schedule output for the first race/deadlock.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.
