# Stateless Concurrency Model Checker with DPOR

[![CI](https://github.com/Aly700/dpor-concurrency-model-checker/actions/workflows/ci.yml/badge.svg)](https://github.com/Aly700/dpor-concurrency-model-checker/actions/workflows/ci.yml)

A deterministic model checker for concurrent programs. The hard boundary is
the happens-before and independence analysis used by Dynamic Partial-Order
Reduction: it must prune equivalent schedules without missing races or
deadlocks. Every reported bug ships a minimal schedule that reproduces it
exactly. Non-termination reports instead ship an intentionally unminimized
lasso witness, because deleting steps cannot in general preserve the repeated
state that proves the cycle.

## What it models

| Domain | Actions | Semantics |
|---|---|---|
| Registers and values | `set`, `bnz`, `assert`, labels | Eight int64 thread-local registers `r0`-`r7`; labels are unscheduled pseudo-actions; assertions fail when the register is zero |
| Plain memory | `read`, `write` | Shared int64 cells, initial 0; conflicting unordered accesses are races |
| Memory models | `--memory-model sc\|tso\|pso`, `fence` | SC by default; TSO adds one FIFO store buffer per thread, PSO uses one FIFO per thread/address, and both expose replayable flush steps plus full drain fences |
| Atomics | `atomic_load`, `atomic_store`, `atomic_rmw`, `cas` | Acquire/release/acq-rel, SC-per-location; atomic-atomic never races, mixed plain/atomic does |
| Mutexes | `lock`, `unlock` | Blocking; release/acquire vector-clock edges; non-owner unlock is a modeled error |
| Condition variables | `wait`, `signal`, `broadcast` | Mesa semantics, two-phase wait (release+sleep, then reacquire); no permit queuing, so lost wakeups deadlock |
| Threads | `spawn`, `join`, `yield` | Static thread bodies with dynamic start; spawn starts a target and creates a happens-before edge, join blocks until a started target finishes and inherits its clock |

Two explorers share one execution semantics:

- `explore_naive` — exhaustive enumeration of all interleavings; the reference oracle.
- `explore_dpor` — Flanagan–Godefroid DPOR with happens-before-aware
  last-point backtracking and sleep sets. Prunes only what the independence
  relation proves commutes (~95% of schedules on deeper programs).

Detection: happens-before data races (vector clocks), deadlocks across all
three blocking kinds (with the wait cycle named), modeled API errors, assertion
failures, and executions that exceed the configured per-thread step bound.
The checker also proves schedule-existence of non-termination when one
execution revisits an exact behavioral state. Safety reports carry
replay-validated, 1-minimal schedules; lasso reports carry an exact stem plus
one cycle.

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

`check` exits `0` for clean programs, including `verdict: clean up to bound`
when at least one execution hit the step bound without a bug; it exits `1` for
races/deadlocks/errors/assertions/non-termination and `2` for usage, parse, or
invalid-schedule errors. Reports are byte-identical across runs.

## Program format

Line-oriented; comments and blank lines allowed; threads numbered in
declaration order:

```text
thread:
  lock m
  write x 1
  unlock m
thread:
  lock m
  read x -> r0
  unlock m
```

Value/control forms:

```text
thread:
  set r1 1
spin:
  atomic_load flag -> r0
  bnz r0 done
  bnz r1 spin
done:
  assert r1
thread:
  atomic_store flag 1
```

The full memory/action grammar is:

```text
set rN IMM
label NAME              # or line label: NAME:
bnz rN NAME
assert rN               # fails when rN == 0
read x -> rN            # legacy "read x" discards the value
write x IMM|rN          # legacy "write x" writes 0
atomic_load f -> rN     # legacy "atomic_load f" discards the value
atomic_store f IMM|rN   # legacy "atomic_store f" stores 0
atomic_rmw f IMM|rN -> rN  # legacy "atomic_rmw f" adds 1 and discards old value
cas f EXPECTED NEW -> rN
fence                   # SC no-op; under TSO/PSO, enabled only after all of this thread's stores drain
```

`dpor check` accepts `--memory-model sc|tso|pso` (default `sc`) and
`--step-bound N` to set the per-thread step bound. Because
backward branches can encode spin loops, a clean verdict is sound only relative
to that bound. If any execution hits the bound, the CLI prints
`verdict: clean up to bound` and `bound_exceeded_executions: N`. Independently,
If a non-repeating execution instead consumes the budget (for example, a loop
that increments a shared counter forever), the bound remains the backstop. If
exploration stops at the schedule cap (`--max-schedules`), the report says
`exploration_capped: true` — a capped verdict is not a verified one.

## Proving divergence

Cycle detection is per execution and path-local. After every transition, the
checker compares an exact canonical byte encoding of the complete behavioral
state: normalized thread PCs, started threads, wait phases and wait sets,
registers, memory values, mutex owners, ordered TSO store buffers, and
canonical per-address PSO FIFO maps. It never
uses a lossy hash; a collision could fabricate a false proof of divergence.
Vector clocks and race history are analysis instrumentation rather than program
behavior, so they are excluded. The resulting witness claims non-termination
only, not repeated race-analysis state.

On a revisit, the report splits the executed schedule at the first occurrence:

```text
verdict: nontermination
cycles_detected: 1
nontermination:
  stem:
    0 0
  cycle:
    0 2
schedule:
  0 0
  0 2
```

The standard `schedule:` block is `stem + one cycle` and replays directly with
`dpor replay`. Replay validates that the end-of-stem and end-of-cycle canonical
states are identical and rejects any schedule that continues after the cycle
closes.

This is an existential scheduling claim: at least one scheduler can repeat the
cycle forever. A peer might remain enabled and, if scheduled, let the program
finish. The report therefore does not claim starvation freedom, fairness, or a
fairness violation. Verdict priority is race/deadlock/error/assertion, then
non-termination, then clean up to bound, then clean; `also_found` preserves
lower-priority findings from the same exploration.

More in `examples/`: data race, AB-BA deadlock, lost wakeup, atomic message
passing, spawn+join pipeline, clean locked counter, unlock error.

## SC/TSO/PSO Memory Models

SC makes each plain write visible immediately. Under `--memory-model tso`, each
thread instead has one FIFO store buffer. Under `--memory-model pso`, each
thread has a separate FIFO for every address: stores to one address remain
ordered, while different addresses may drain in either order. `write x V`
enqueues and an internal `flush x` transition commits the oldest eligible
value to shared memory.

Flushes are printed in traces and use reserved schedule action index
`4294967295`. TSO retains the existing two-number step. A PSO flush adds the
canonical numeric program-address ID as a third number, so replay validates
the exact address choice:

```text
0 4294967295 1
```

Store buffering is therefore observable:

```text
thread:
  write x 1
  read y -> r0
thread:
  write y 1
  read x -> r1
```

The outcome `r0 == 0 && r1 == 0` is reachable under both TSO and PSO when both
reads run before either buffer flushes. Adding `fence` after each write drains
all of that thread's buffers before the read. The IR intentionally provides one
full fence; a separate `sfence` would add no behavior while loads are not
reordered and is left as future work.

Message passing distinguishes PSO from TSO:

```text
thread:
  write data 1
  write flag 1
thread:
  read flag -> r0
  # if r0 != 0, read data and assert it is nonzero
```

| Outcome: observe `flag == 1`, then `data == 0` | SC | TSO | PSO |
|---|---:|---:|---:|
| Without a fence | unreachable | unreachable | reachable |
| Fence between the writes | unreachable | unreachable | unreachable |

TSO's single FIFO cannot publish `flag` before `data`; PSO can flush the flag
address first. A fence between the stores drains `data` before `flag` can
enqueue. Plain accesses are still checked for happens-before races, so these
litmus reports contain a primary race verdict plus an assertion witness where
the relaxed value outcome is reachable.

`examples/classic/peterson_tso*.dpor` and `dekker_tso*.dpor` are bounded
entry-check witnesses for plain flag/turn coordination under TSO. The unfenced
files reach the assertion in TSO; the fenced files remove that assertion in the
bounded witness. Because this checker treats plain flag/turn accesses as data
races, these plain-coordinate demos are not clean SC proofs; the race-free SC
gallery uses atomic coordination variables instead.

## Classic algorithms

`examples/classic/` contains a checked gallery of classic mutual-exclusion and
lock-free patterns: Peterson, Dekker, a bounded two-thread Bakery
simplification, a Treiber push skeleton, and a failed-CAS handoff. Each model is
paired with a deliberately broken variant and documented in
[`examples/classic/README.md`](examples/classic/README.md), including the exact
bounded verdict and any `.dpor` modeling limitation.

## Verification gates

DPOR is never trusted on faith. Deterministic gates assert that
`explore_dpor` and the exhaustive oracle agree on race/deadlock/error/assertion
and cycle existence and on whether any execution hit the step bound, that DPOR never
explores more schedules, that every DPOR report replays to an identical report,
and how far DPOR is from one schedule per Mazurkiewicz class:

1. **Exhaustive 2-thread sweep** — every program over a 17-action alphabet
   (capped per length pair; ~21k programs).
2. **Strided 3-thread sweep** — 65,542 programs evenly sampled from the full
   15-action, 6-slot space.
3. **Seeded differential fuzz** — 3,000 random 2–5-thread programs per run,
   including spawn-shaped, value/branch/CAS/assertion programs, exact spin
   cycles, growing-state bound backstops, and deliberately malformed ones;
   failures print the seed and the program in `.dpor` syntax for by-hand
   reproduction. Deterministic fractions run under TSO and PSO, and the summary
   prints both model counts plus naive/DPOR cycle counters.
4. **Optimality meter** — collects naive schedules for small non-error,
   non-assertion programs, canonicalizes phase-aware Mazurkiewicz trace
   classes using the same transition predicate DPOR prunes with, asserts
   `classes <= dpor <= naive`, and prints the aggregate DPOR/classes
   redundancy ratio.
5. **Buffered-model oracles** — capped TSO and PSO program sweeps compare
   naive and DPOR verdict/cycle existence, schedule dominance, and exact replay.
6. **Cross-model inclusion** — a deterministic two-thread corpus plus
   fixed-seed samples checks per-kind bug existence is monotone
   `SC => TSO => PSO`, skipping and reporting any truncated exploration.

All gates are deterministic and run in CI on Linux and macOS.

## Design records

Architecture in `ARCHITECTURE.md`, invariants in `INVARIANTS.md`, and every
soundness-relevant decision in `adr/` (0001 architecture crux through 0017
PSO), including the exact vector-clock edge for each
synchronization kind and why each DPOR pruning step cannot lose a bug class.

**[docs/case-study.md](docs/case-study.md)** tells the verification story:
how the differential gates caught two real DPOR soundness bugs that had
survived review, and why two attempted improvements were deferred with
measurements instead of shipped.
