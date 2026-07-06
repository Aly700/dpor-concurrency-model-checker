# Stateless Concurrency Model Checker with DPOR

## Scope

A deterministic model checker for concurrent programs whose hard boundary is the happens-before and independence analysis used by Dynamic Partial-Order Reduction: it must prune equivalent schedules without missing races or deadlocks.

## Stack

C++20, CMake, cooperative scheduler over a small controlled action IR.

## Build and smoke test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Phase map

1. Cooperative scheduler and deterministic replay.
2. Naive exhaustive interleaving enumeration on tiny programs.
3. Vector-clock happens-before tracking.
4. DPOR pruning with sound independence predicates.
5. Deadlock/race reports with minimal reproducing schedules.

