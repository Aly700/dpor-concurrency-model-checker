# Stateless Concurrency Model Checker with DPOR

[![CI](https://github.com/Aly700/dpor-concurrency-model-checker/actions/workflows/ci.yml/badge.svg)](https://github.com/Aly700/dpor-concurrency-model-checker/actions/workflows/ci.yml)

A deterministic model checker for concurrent programs. The hard boundary is
the happens-before and independence analysis used by Dynamic Partial-Order
Reduction: it must prune equivalent schedules without missing races or
deadlocks. Every reported bug ships a minimal schedule that reproduces it
exactly.

## What it models

| Domain | Actions | Semantics |
|---|---|---|
| Plain memory | `read`, `write` | Conflicting unordered accesses are races |
| Atomics | `atomic_load`, `atomic_store`, `atomic_rmw` | Acquire/release/acq-rel, SC-per-location; atomic-atomic never races, mixed plain/atomic does |
| Mutexes | `lock`, `unlock` | Blocking; release/acquire vector-clock edges; non-owner unlock is a modeled error |
| Condition variables | `wait`, `signal`, `broadcast` | Mesa semantics, two-phase wait (release+sleep, then reacquire); no permit queuing, so lost wakeups deadlock |
| Threads | `spawn`, `join`, `yield` | Static thread bodies with dynamic start; spawn starts a target and creates a happens-before edge, join blocks until a started target finishes and inherits its clock |

Two explorers share one execution semantics:

- `explore_naive` — exhaustive enumeration of all interleavings; the reference oracle.
- `explore_dpor` — Flanagan–Godefroid DPOR with happens-before-aware
  last-point backtracking and sleep sets. Prunes only what the independence
  relation proves commutes (~95% of schedules on deeper programs).

Detection: happens-before data races (vector clocks), deadlocks across all
three blocking kinds (with the wait cycle named), and modeled API errors.
Reports carry replay-validated, 1-minimal reproducing schedules.

## Quick start

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
./build/dpor check examples/lost_wakeup.dpor
```

```text
verdict: deadlock
schedules_explored: 4
deadlock:
  blocked:
    thread 0: condition cv mutex m
trace:
  0: thread 0 action 0 lock m
  1: thread 0 action 1 wait cv m (sleep)
  2: thread 1 action 0 signal cv
  ...
schedule:
  0 0
  0 1
  ...
```

The lines after `schedule:` replay directly:

```bash
./build/dpor check examples/data_race.dpor > race.report
awk '/^schedule:$/ {copy=1; next} copy {print}' race.report > race.schedule
./build/dpor replay examples/data_race.dpor --schedule race.schedule   # identical report
```

`check` exits `0` for clean programs, `1` for races/deadlocks/errors, `2` for
usage, parse, or invalid-schedule errors. Reports are byte-identical across
runs.

## Program format

Line-oriented; comments and blank lines allowed; threads numbered in
declaration order:

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

More in `examples/`: data race, AB-BA deadlock, lost wakeup, atomic message
passing, spawn+join pipeline, clean locked counter, unlock error.

## Verification gates

DPOR is never trusted on faith. Three differential gates assert that
`explore_dpor` and the exhaustive oracle agree on race/deadlock/error
verdicts, that DPOR never explores more schedules, and that every DPOR
report replays to an identical report:

1. **Exhaustive 2-thread sweep** — every program over a 17-action alphabet
   (capped per length pair; ~21k programs).
2. **Strided 3-thread sweep** — 65,542 programs evenly sampled from the full
   15-action, 6-slot space.
3. **Seeded differential fuzz** — 3,000 random 2–5-thread programs per run,
   including spawn-shaped and deliberately malformed ones; failures print the
   seed and the program in `.dpor` syntax for by-hand reproduction.

All gates are deterministic and run in CI on Linux and macOS.

## Design records

Architecture in `ARCHITECTURE.md`, invariants in `INVARIANTS.md`, and every
soundness-relevant decision in `adr/` (0001 architecture crux through 0012
source-DPOR deferral), including the exact vector-clock edge for each
synchronization kind and why each DPOR pruning step cannot lose a bug class.

**[docs/case-study.md](docs/case-study.md)** tells the verification story:
how the differential gates caught two real DPOR soundness bugs that had
survived review, and why two attempted improvements were deferred with
measurements instead of shipped.
