#!/usr/bin/env python3
"""Generate deterministic showcase evidence from the real checker binary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
from typing import Any


PROVENANCE = {"CURATED", "MEASURED", "MEASURED_ENVIRONMENT_DEPENDENT"}
BUG_KINDS = ("race", "deadlock", "error", "assertion", "nontermination")


def canonical_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def atomic_write(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(data)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary_path = pathlib.Path(temporary.name)
    os.replace(temporary_path, path)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_config(config: dict[str, Any], source_dir: pathlib.Path) -> None:
    ids: set[str] = set()
    programs = config.get("programs", [])
    for program in programs:
        program_id = program.get("id")
        if not isinstance(program_id, str) or not program_id:
            raise ValueError("program id must be a nonempty string")
        if program_id in ids:
            raise ValueError(f"duplicate program id: {program_id}")
        ids.add(program_id)

    run_ids: set[str] = set()
    for program in programs:
        program_id = program["id"]
        source = source_dir / program.get("source", "")
        if not source.is_file():
            raise ValueError(f"missing curated source: {source}")
        seen_models: set[str] = set()
        for run in program.get("runs", []):
            model = run.get("memory_model")
            if model not in {"sc", "tso", "pso"}:
                raise ValueError(f"invalid memory model for {program_id}: {model}")
            if model in seen_models:
                raise ValueError(f"duplicate run model for {program_id}: {model}")
            seen_models.add(model)
            run_id = f"{program_id}-{model}"
            if run_id in run_ids:
                raise ValueError(f"duplicate run id: {run_id}")
            run_ids.add(run_id)
            if not isinstance(run.get("step_bound"), int) or run["step_bound"] <= 0:
                raise ValueError(f"invalid step bound for {run_id}")
            if not isinstance(run.get("max_schedules"), int) or run["max_schedules"] <= 0:
                raise ValueError(f"invalid max schedules for {run_id}")
    for comparison in config.get("model_comparisons", []):
        if comparison.get("program_id") not in ids:
            raise ValueError("model comparison names an unknown program")


def validate_export(exported: dict[str, Any], run_id: str) -> None:
    runs = exported.get("runs", {})
    for explorer in ("naive", "dpor"):
        if explorer not in runs:
            raise ValueError(f"{run_id} omitted {explorer} run")
        if runs[explorer].get("exploration_capped"):
            raise ValueError(f"{run_id} {explorer} exploration was capped")
    pruning = exported.get("pruning", {})
    naive = pruning.get("naive_equivalent_schedules")
    dpor = pruning.get("dpor_schedules_explored")
    pruned = pruning.get("schedules_pruned")
    if not all(isinstance(value, int) for value in (naive, dpor, pruned)):
        raise ValueError(f"{run_id} omitted integer pruning counts")
    if dpor + pruned != naive:
        raise ValueError(f"{run_id} pruning counts do not conserve schedules")
    if runs["naive"].get("schedules_explored") != naive:
        raise ValueError(f"{run_id} naive count differs from pruning ledger")
    if runs["dpor"].get("schedules_explored") != dpor:
        raise ValueError(f"{run_id} DPOR count differs from pruning ledger")


def validate_expected(exported: dict[str, Any], expected: dict[str, Any], run_id: str) -> None:
    outcome = exported["evidence"]["run_outcome"]
    if outcome["primary_verdict"] != expected["primary_verdict"]:
        raise ValueError(
            f"{run_id} expected {expected['primary_verdict']} but measured "
            f"{outcome['primary_verdict']}"
        )
    for kind in BUG_KINDS:
        if outcome["bug_shape"][kind] is not expected[kind]:
            raise ValueError(f"{run_id} measured {kind} shape diverged")


def deterministic_run(run: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value
        for key, value in run.items()
        if key not in {"wall_clock_us", "wall_clock_note"}
    }


def schedule_bytes(schedule: list[dict[str, Any]]) -> bytes:
    lines: list[str] = []
    for step in schedule:
        line = f"{step['thread']} {step['action_index']}"
        if step["flush_address_id"] is not None:
            line += f" {step['flush_address_id']}"
        lines.append(line)
    return (("\n".join(lines) + "\n") if lines else "").encode("utf-8")


def run_exporter(
    exporter: pathlib.Path,
    source: pathlib.Path,
    program_id: str,
    run: dict[str, Any],
) -> dict[str, Any]:
    command = [
        str(exporter),
        "--program",
        str(source),
        "--program-id",
        program_id,
        "--memory-model",
        run["memory_model"],
        "--step-bound",
        str(run["step_bound"]),
        "--max-schedules",
        str(run["max_schedules"]),
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"exporter failed for {program_id}-{run['memory_model']}: "
            f"{completed.stderr.strip()}"
        )
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"exporter emitted invalid JSON: {error}") from error


def artifact_record(
    path: str,
    provenance: str,
    deterministic: bool,
    contains: str,
    data: bytes | None,
    **metadata: Any,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "path": path,
        "provenance": provenance,
        "deterministic": deterministic,
        "contains": contains,
        "bytes": len(data) if deterministic and data is not None else None,
        "sha256": sha256_bytes(data) if deterministic and data is not None else None,
    }
    record.update(metadata)
    return record


def build_model_comparison(
    program_id: str,
    records: dict[str, dict[str, Any]],
    baseline_model: str,
    variant_model: str,
) -> dict[str, Any]:
    if baseline_model not in records or variant_model not in records:
        raise ValueError(f"{program_id} model comparison omitted a configured model")
    before = records[baseline_model]["property"]["verdict"]
    after = records[variant_model]["property"]["verdict"]
    if before == after:
        raise ValueError(
            f"{program_id} declared model-sensitive property does not flip: {before}"
        )
    return {
        "schema_version": 1,
        "provenance": "MEASURED",
        "program_id": program_id,
        "property": "assertion_reachability",
        "baseline_model": baseline_model,
        "variant_model": variant_model,
        "flip": {"from": before, "to": after},
        "records": records,
        "interpretation": (
            "The assertion-reachability property flips. The primary checker verdict "
            "remains race in both models because the plain accesses are intentionally racy."
        ),
    }


def validate_manifest(manifest: dict[str, Any]) -> None:
    seen: set[str] = set()
    for artifact in manifest.get("artifacts", []):
        path = artifact.get("path")
        if path in seen:
            raise ValueError(f"duplicate manifest artifact: {path}")
        seen.add(path)
        if artifact.get("provenance") not in PROVENANCE:
            raise ValueError(f"invalid provenance for {path}")
        if artifact.get("deterministic"):
            if not artifact.get("sha256"):
                raise ValueError(f"deterministic artifact missing sha256: {path}")
            if not isinstance(artifact.get("bytes"), int):
                raise ValueError(f"deterministic artifact missing byte size: {path}")
        elif artifact.get("provenance") != "MEASURED_ENVIRONMENT_DEPENDENT":
            raise ValueError(f"only environment-dependent artifacts may be nondeterministic: {path}")


def generate(
    source_dir: pathlib.Path,
    output_dir: pathlib.Path,
    exporter: pathlib.Path,
    config_path: pathlib.Path,
) -> dict[str, Any]:
    config_bytes = config_path.read_bytes()
    config = json.loads(config_bytes)
    validate_config(config, source_dir)
    if not exporter.is_file():
        raise ValueError(f"missing showcase exporter binary: {exporter}")

    artifacts: list[dict[str, Any]] = []
    manifest_programs: list[dict[str, Any]] = []
    comparisons: dict[str, dict[str, dict[str, Any]]] = {}
    stats_runs: list[dict[str, Any]] = []

    output_config = output_dir / "config.json"
    atomic_write(output_config, config_bytes)
    artifacts.append(
        artifact_record(
            "config.json",
            "CURATED",
            True,
            "The selected teaching corpus, complete bounds, and expected semantic shapes.",
            config_bytes,
            source="showcase/config.json",
        )
    )

    for program in config["programs"]:
        program_id = program["id"]
        source_relative = program["source"]
        source_path = source_dir / source_relative
        program_bytes = source_path.read_bytes()
        program_artifact = f"programs/{program_id}.dpor"
        atomic_write(output_dir / program_artifact, program_bytes)
        artifacts.append(
            artifact_record(
                program_artifact,
                "CURATED",
                True,
                f"Exact source text for {program['title']}.",
                program_bytes,
                program_id=program_id,
                source=source_relative,
            )
        )
        program_runs: list[str] = []

        for run in program["runs"]:
            model = run["memory_model"]
            run_id = f"{program_id}-{model}"
            exported = run_exporter(exporter, source_path, program_id, run)
            validate_export(exported, run_id)
            validate_expected(exported, run["expected"], run_id)
            program_runs.append(run_id)

            tree_path = f"trees/{run_id}.json"
            tree_document = {
                "schema_version": 1,
                "provenance": "MEASURED",
                "program_id": program_id,
                "memory_model": model,
                "configuration": exported["configuration"],
                "runs": {
                    explorer: deterministic_run(exported["runs"][explorer])
                    for explorer in ("naive", "dpor")
                },
                "pruning": exported["pruning"],
                "tree": exported["tree"],
            }
            tree_bytes = canonical_json_bytes(tree_document)
            atomic_write(output_dir / tree_path, tree_bytes)
            artifacts.append(
                artifact_record(
                    tree_path,
                    "MEASURED",
                    True,
                    f"Exact {model.upper()} DPOR tree with counted naive-only PRUNED boundaries for {program['title']}.",
                    tree_bytes,
                    program_id=program_id,
                    memory_model=model,
                    run_id=run_id,
                )
            )

            evidence_path = f"evidence/{run_id}.json"
            evidence_document = {
                "schema_version": 1,
                "provenance": "MEASURED",
                "program_id": program_id,
                "program_source": program_artifact,
                "memory_model": model,
                "configuration": exported["configuration"],
                "evidence": exported["evidence"],
            }
            evidence_bytes = canonical_json_bytes(evidence_document)
            atomic_write(output_dir / evidence_path, evidence_bytes)
            artifacts.append(
                artifact_record(
                    evidence_path,
                    "MEASURED",
                    True,
                    f"Checker verdicts, minimal witnesses, effects, and vector clocks for {program['title']} under {model.upper()}.",
                    evidence_bytes,
                    program_id=program_id,
                    memory_model=model,
                    run_id=run_id,
                )
            )

            for witness in exported["evidence"]["witnesses"]:
                kind = witness["kind"]
                schedule_path = f"schedules/{run_id}-{kind}.schedule"
                data = schedule_bytes(witness["schedule"])
                atomic_write(output_dir / schedule_path, data)
                artifacts.append(
                    artifact_record(
                        schedule_path,
                        "MEASURED",
                        True,
                        f"CLI-replayable minimal {kind} schedule for {program['title']} under {model.upper()}.",
                        data,
                        program_id=program_id,
                        memory_model=model,
                        run_id=run_id,
                        witness_kind=kind,
                        evidence=evidence_path,
                    )
                )

            stats_runs.append(
                {
                    "run_id": run_id,
                    "program_id": program_id,
                    "memory_model": model,
                    "configuration": exported["configuration"],
                    "naive": exported["runs"]["naive"],
                    "dpor": exported["runs"]["dpor"],
                    "pruning": exported["pruning"],
                }
            )
            comparisons.setdefault(program_id, {})[model] = {
                "primary_verdict": exported["evidence"]["run_outcome"]["primary_verdict"],
                "bug_shape": exported["evidence"]["run_outcome"]["bug_shape"],
                "property": exported["evidence"]["property"],
                "tree_artifact": tree_path,
                "evidence_artifact": evidence_path,
            }

        manifest_programs.append(
            {
                "id": program_id,
                "title": program["title"],
                "teaching_role": program["teaching_role"],
                "source": source_relative,
                "exported_source": program_artifact,
                "runs": program_runs,
            }
        )

    for comparison in config.get("model_comparisons", []):
        program_id = comparison["program_id"]
        document = build_model_comparison(
            program_id,
            comparisons[program_id],
            comparison["baseline_model"],
            comparison["variant_model"],
        )
        path = f"evidence/{program_id}-model-comparison.json"
        data = canonical_json_bytes(document)
        atomic_write(output_dir / path, data)
        artifacts.append(
            artifact_record(
                path,
                "MEASURED",
                True,
                "Paired SC/PSO records proving the assertion-reachability property flip.",
                data,
                program_id=program_id,
                baseline_model=comparison["baseline_model"],
                variant_model=comparison["variant_model"],
            )
        )

    stats_document = {
        "schema_version": 1,
        "provenance": "MEASURED_ENVIRONMENT_DEPENDENT",
        "wall_clock_policy": (
            "wall_clock_us is measured with a monotonic host clock and is excluded "
            "from byte-determinism checks; all other fields are checked after normalization"
        ),
        "runs": stats_runs,
    }
    stats_path = "stats/runs.json"
    stats_bytes = canonical_json_bytes(stats_document)
    atomic_write(output_dir / stats_path, stats_bytes)
    artifacts.append(
        artifact_record(
            stats_path,
            "MEASURED_ENVIRONMENT_DEPENDENT",
            False,
            "Per-run schedules, pruned schedules, prefix states, depth, and environment-dependent wall clock.",
            None,
        )
    )

    artifacts.sort(key=lambda artifact: artifact["path"])
    summary = {label: 0 for label in sorted(PROVENANCE)}
    for artifact in artifacts:
        summary[artifact["provenance"]] += 1
    manifest = {
        "schema_version": 1,
        "discipline": {
            "CURATED": "Human-selected input programs and corpus configuration; never represented as measurements.",
            "MEASURED": "Real checker output recomputed at export time and byte-deterministic.",
            "MEASURED_ENVIRONMENT_DEPENDENT": "Real checker timing plus deterministic counters; wall clock is excluded from byte comparison.",
        },
        "generator": {
            "command": "python3 showcase/generate.py --output showcase --exporter build/dpor_showcase_export",
            "checker_exporter": "build/dpor_showcase_export",
            "replay_cli": "build/dpor",
        },
        "programs": manifest_programs,
        "artifacts": artifacts,
        "summary": {"artifact_count": len(artifacts), "by_provenance": summary},
    }
    validate_manifest(manifest)
    atomic_write(output_dir / "manifest.json", canonical_json_bytes(manifest))
    return manifest


def parse_args() -> argparse.Namespace:
    script_dir = pathlib.Path(__file__).resolve().parent
    source_dir = script_dir.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path, default=source_dir)
    parser.add_argument("--output", type=pathlib.Path, default=script_dir)
    parser.add_argument(
        "--exporter",
        type=pathlib.Path,
        default=source_dir / "build" / "dpor_showcase_export",
    )
    parser.add_argument("--config", type=pathlib.Path, default=script_dir / "config.json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest = generate(
        args.source_dir.resolve(),
        args.output.resolve(),
        args.exporter.resolve(),
        args.config.resolve(),
    )
    print(
        "showcase generate: PASS "
        f"programs={len(manifest['programs'])} "
        f"artifacts={manifest['summary']['artifact_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
