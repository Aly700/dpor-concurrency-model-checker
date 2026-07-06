# Architecture — DPOR Model Checker

## Pipeline

```text
controlled concurrent program -> scheduler -> explored schedules -> HB/race/deadlock analysis -> minimal replay
```

## Current scaffold

- `model::Program` is a tiny action IR.
- `ModelChecker::explore_naive` is the exhaustive oracle.
- `VectorClock` is the base happens-before data structure.
- DPOR should be added beside, not instead of, the naive oracle.

## Design bias

The checker should first be obviously correct on tiny programs. Reduction is valuable only while the naive oracle can still validate it on small state spaces.
