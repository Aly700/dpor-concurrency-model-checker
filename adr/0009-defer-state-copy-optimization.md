# ADR 0009: Defer the backtrackable-state optimization

## Status

Superseded by ADR 0023 after the recorded revisit conditions fired.

## Context

Both explorers copy the full `ExecutionState` per DFS branch. An apply/undo
log would remove those copies, but it must restore lossy mutations exactly
(vector-clock joins, `reads_since_last_write` clears, atomic location clock
replacement, broadcast wake-ups), concentrating new complexity in the most
invariant-dense file.

## Decision

Measured in Release mode, the two heaviest gates run in ~3.5s each
(65,540-program sweep; 3,000-program fuzz). The rewrite would buy seconds of
CI time while weakening the "obviously correct on tiny programs" design bias
(ARCHITECTURE.md). We defer it and instead default `CMAKE_BUILD_TYPE` to
Release, which removes the only observed pain (the ~10x slower unoptimized
default build).

## Revisit when

Programs grow beyond the current gate sizes (longer traces, more threads),
or a profiling run shows state copying dominating a real workload. Any
future implementation must reproduce byte-identical gate summary lines
(program counts, schedule totals, per-kind bug counts) for both explorers.

## Invariant protected

Soundness via simplicity: the exploration core stays small enough to audit
against INVARIANTS.md line by line.
