# ADR 0032: Read-only showcase observations and provenance export

## Status

Accepted.

## Context

The portfolio world needs branching schedule trees, DPOR pruning ratios,
structured bug witnesses, vector-clock causality, and cross-memory-model
records. Rendering those from handwritten fixtures or a second interpreter
would violate the project's replay and determinism invariants. Scraping the
existing text report is also insufficient: it exposes one primary trace but
not the explorer's terminal representatives, enabled choices, state effects,
or a secondary assertion witness hidden behind a primary race.

ADR 0031 separately rejected independently checkable pruning certificates.
This export must not imply that stronger theorem. It may compare the exact
schedule sets produced by complete naive and DPOR runs, but it does not certify
each independence claim inside DPOR.

## Decision

Add two narrow `ModelChecker` diagnostics:

- `collect_dpor_schedules(max)` runs the existing DPOR traversal with an
  optional terminal sink. The ordinary `explore_dpor` path passes no sink.
  Every increment of the explored-outcome count has one matching collected
  schedule. A step-bound outcome records the attempted enabled endpoint, just
  like the existing naive collector, without claiming it executed.
- `inspect_schedule(schedule)` uses the replay validator and interpreter to
  expose, per numeric step, the enabled set before selection, effective action,
  dense pre/post vector clock, changed registers, changed shared values, and
  changed store buffers. Its `executed` bit is false only for the terminal
  bound-attempt representation.

Keep serialization and corpus policy out of the model library. The separate
`dpor_showcase_export` executable builds one comparison record from complete
naive and DPOR runs. It expands DPOR prefixes and collapses each earliest
naive-only branch into one counted `PRUNED` boundary. The boundaries partition
the naive schedules absent from DPOR, so their counts must equal
`naive_equivalent - dpor_explored`.

The Python generator owns the curated six-program configuration, exact source
copies, canonical JSON, numeric schedule files, and manifest. The verifier
incrementally rebuilds the real binaries, regenerates in a temporary directory,
byte-compares deterministic artifacts and the manifest, validates tree/count
conservation, and sends every witness schedule through the existing
`dpor replay --schedule` CLI.

Use three provenance labels:

- `CURATED`: selected input source and run configuration;
- `MEASURED`: deterministic output recomputed from the checker;
- `MEASURED_ENVIRONMENT_DEPENDENT`: the stats ledger containing monotonic
  wall-clock duration.

Only `wall_clock_us` is removed for normalized stats comparison. Wall time
never enters core state, schedule choice, bug detection, artifact hashes, or
the manifest's deterministic bytes.

## Invariants protected

- Every displayed witness is a checker report schedule and passes exact CLI
  replay; secondary witnesses also store the primary report produced on their
  particular replay path.
- Every displayed action, enabled choice, state effect, and clock comes from
  the existing interpreter after ordinary replay validation.
- Terminal collection is observational. With no collector, the established
  exploration path and optimality meter are unchanged.
- A `PRUNED` node means “this complete naive subtree has no DPOR terminal
  representative,” with an exact measured schedule count. It is not relabeled
  as a semantic certificate.
- Capped explorations cannot produce exports.
- The manifest never calls curated source measured, and never calls wall-clock
  deterministic.

## Consequences

The showcase can render real bounded schedule worlds without depending on
private `ExecutionState` layout or duplicating concurrency semantics. Export
runs execute each explorer and collector separately, using additional time and
memory proportional to their terminal schedule lists; this cost exists only in
the offline showcase target. Large naive trees remain legible because omitted
branches are counted at their first DPOR boundary rather than serialized in
full.

The diagnostic API is public C++ surface and therefore has focused tests. Any
new terminal disposition or execution-state effect must extend the observation
contract and verifier before the showcase can claim it.
