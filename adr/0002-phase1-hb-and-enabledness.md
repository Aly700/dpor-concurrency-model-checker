# ADR 0002: Phase 1 Happens-Before and Enabledness

## Status

Accepted.

## Context

The Phase 1 checker must be a correctness reference for future DPOR work. The
old implementation enumerated all thread program-counter interleavings but did
not model lock enabledness and detected races with an adjacent-action heuristic.
That violated the soundness, replay, and happens-before invariants.

## Decision

Use an explicit small-step interpreter state during both DFS and replay:

- Per-thread program counters protect the replay invariant that a schedule is a
  deterministic sequence of `(thread, action_index)` steps.
- Ordered `std::map` mutex ownership and mutex vector clocks protect the
  determinism invariant and make lock enabledness explicit.
- Per-thread vector clocks, last-write metadata, and reads-since-last-write
  metadata protect the happens-before invariant.
- DFS enumerates enabled actions in ascending thread-id order. It may explore
  too many schedules, but it does not skip enabled schedules in the naive
  oracle.
- `Lock` on a held mutex is disabled. `Unlock` by a non-owner is an executable
  modeled error report, not undefined behavior.
- A state with unfinished threads and no enabled action is reported as a
  deadlock; finished threads are not reported as blocked waiters.
- Replay uses the same step function as DFS and adds validation that rejects
  out-of-range, out-of-order, and disabled schedule steps.

## Consequences

Race detection is now happens-before based instead of adjacency based.
Mutex-protected accesses are ordered by release/acquire edges and no longer
produce false races.

The naive oracle remains the reference semantics. Because it explores all
enabled interleavings, per-execution happens-before race detection across all
visited schedules is complete for the modeled IR: any reachable modeled race has
some explored prefix whose second conflicting access reports it, and any
reachable deadlock has some explored prefix with unfinished blocked threads and
no enabled action.

The independence predicate is intentionally conservative. It marks conflicting
same-address memory operations and same-mutex operations dependent, and only
classifies pairs independent when their enabled transitions commute with respect
to modeled state and future enabledness. Future action kinds must extend this
predicate with equally explicit soundness arguments before DPOR can rely on
them.
