# Stateless Concurrency Model Checker with DPOR

## Scope

A deterministic model checker for concurrent programs whose hard boundary is the happens-before and independence analysis used by Dynamic Partial-Order Reduction: it must prune equivalent schedules without missing races or deadlocks.

## Stack

C++20, CMake, cooperative scheduler over a small controlled action IR.

## Build and smoke test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## CLI

The build produces a single executable, `dpor`, with `check` and `replay`
subcommands:

```bash
./build/dpor check examples/data_race.dpor
./build/dpor check examples/ab_ba_deadlock.dpor --explorer naive
./build/dpor check examples/clean_locked_counter.dpor --max-schedules 1000
```

The text format is line-oriented. Comments and blank lines are allowed, and
threads are numbered in declaration order:

```text
thread:
  lock m
  write x
  unlock m
thread:
  lock m
  read x
  unlock m
```

Bug reports end with a `schedule:` block. The lines after that marker can be
replayed directly:

```bash
./build/dpor check examples/data_race.dpor > race.report
awk '/^schedule:$/ {copy=1; next} copy {print}' race.report > race.schedule
./build/dpor replay examples/data_race.dpor --schedule race.schedule
```

`check` exits `0` for clean programs and `1` for races, deadlocks, or modeled
errors. Usage errors, parser errors, and invalid replay schedules exit `2`.

## Phase map

1. Cooperative scheduler and deterministic replay.
2. Naive exhaustive interleaving enumeration on tiny programs.
3. Vector-clock happens-before tracking.
4. DPOR pruning with sound independence predicates.
5. Deadlock/race reports with minimal reproducing schedules.

