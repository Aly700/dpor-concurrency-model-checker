# Branching-Timelines Showcase Export

This directory is a deterministic evidence boundary for a portfolio world. It contains no invented schedules, counts, tree branches, vector clocks, or verdicts.

## Provenance discipline

- `CURATED` means a human selected the teaching input. `config.json` and `programs/*.dpor` have this label. Exported program files are exact byte copies of their declared files under `examples/`.
- `MEASURED` means `dpor_showcase_export` recomputed the artifact from the real checker. Trees, evidence, schedules, and the SC/PSO comparison have this label and carry SHA-256 digests in `manifest.json`.
- `MEASURED_ENVIRONMENT_DEPENDENT` means the values are real but not reproducible bytes. Only `stats/runs.json` has this label because it includes monotonic wall-clock durations. Its schedule, pruning, state, and depth fields are still compared after removing only `wall_clock_us`.

The manifest is generated last. It records each data artifact's label, purpose, byte size, digest (when deterministic), source or run identity, and a summary by provenance.

## Curated teaching set

The six programs are the minimal race, intact and wounded Dekker models, a condition-variable lost wakeup, a test-and-set spin lasso, and the existing PSO message-passing discriminator. Their full source text is under `programs/`.

`mp_pso` needs one careful reading: its primary checker verdict is `race` under both SC and PSO because the plain accesses are intentionally racy. The measured property verdict flips from assertion reachability `holds` under SC to `violated` under PSO. `evidence/mp_pso-model-comparison.json` preserves both facts instead of hiding the race behind a friendlier label.

## Tree representation

Each `trees/<program>-<model>.json` is a complete pruning comparison:

- every DPOR-reached numeric schedule prefix is an `EXPLORED` node;
- its enabled endpoints and enabled thread IDs come from interpreter replay;
- every edge contains the exact numeric endpoint and effective checker action;
- a naive-only branch is represented by one `PRUNED` boundary node with the exact number of full naive schedules below it;
- the PRUNED boundaries are disjoint and sum to `naive_equivalent_schedules - dpor_schedules_explored`.

This representation keeps the files legible without pretending the omitted naive subtrees do not exist. `prefix_states_visited` is the exact number of distinct numeric schedule prefixes in that explorer's tree, and `max_depth` is the longest collected terminal schedule.

## Bug evidence

Each `evidence/<program>-<model>.json` contains every bug kind found by the DPOR run. A witness records:

- the checker-minimized schedule (or exact nontermination lasso);
- structured steps with thread, endpoint, effective action, enabled choices, address/register/store-buffer mutations, and pre/post vector clocks;
- all prior steps ordered before each step, derived directly from those clocks;
- race endpoints and their clocks, deadlock blockers, assertion details, or lasso stem/cycle/fairness;
- the exact primary report produced when that particular witness schedule is replayed. This matters when a secondary assertion schedule encounters a primary race first.

Every `schedules/*.schedule` file is the numeric witness input accepted by `dpor replay --schedule`.

## Generate and verify

From the repository root:

```bash
cmake -S . -B build
cmake --build build --target dpor dpor_showcase_export -j8
python3 showcase/generate.py --output showcase --exporter build/dpor_showcase_export
python3 showcase/verify.py
```

The verifier incrementally rebuilds the two binaries, validates manifest hashes and curated source identity, regenerates into a temporary directory, byte-compares every deterministic artifact and the manifest, compares normalized stats, checks tree conservation, and replays all exported schedules through the existing CLI. Any divergence names the failing artifact and exits nonzero.
