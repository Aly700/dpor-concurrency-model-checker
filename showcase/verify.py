#!/usr/bin/env python3
"""Rebuild, regenerate, byte-compare, and replay the showcase export."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import subprocess
import tempfile
from typing import Any

try:
    from showcase import generate
except ImportError:  # Direct execution places showcase/ itself on sys.path.
    import generate  # type: ignore[no-redef]


def validate_manifest(manifest: dict[str, Any]) -> None:
    generate.validate_manifest(manifest)
    artifacts = manifest.get("artifacts", [])
    expected_count = manifest.get("summary", {}).get("artifact_count")
    if expected_count != len(artifacts):
        raise ValueError("manifest artifact_count does not match its artifact list")
    measured = {label: 0 for label in generate.PROVENANCE}
    for artifact in artifacts:
        measured[artifact["provenance"]] += 1
    if manifest.get("summary", {}).get("by_provenance") != {
        label: measured[label] for label in sorted(measured)
    }:
        raise ValueError("manifest provenance summary does not match its artifacts")


def verify_declared_artifacts(
    source_dir: pathlib.Path,
    showcase_dir: pathlib.Path,
    manifest: dict[str, Any],
) -> None:
    validate_manifest(manifest)
    for artifact in manifest["artifacts"]:
        relative = artifact["path"]
        path = showcase_dir / relative
        if not path.is_file():
            raise ValueError(f"missing declared artifact: {relative}")
        data = path.read_bytes()
        if artifact["deterministic"]:
            if len(data) != artifact["bytes"]:
                raise ValueError(f"byte-size divergence: {relative}")
            digest = hashlib.sha256(data).hexdigest()
            if digest != artifact["sha256"]:
                raise ValueError(f"sha256 divergence: {relative}")
        if artifact["provenance"] == "CURATED":
            source_relative = artifact.get("source")
            if not isinstance(source_relative, str):
                raise ValueError(f"curated artifact omits source: {relative}")
            source = source_dir / source_relative
            if not source.is_file():
                raise ValueError(f"curated source is missing: {relative}")
            if data != source.read_bytes():
                raise ValueError(f"curated source divergence: {relative}")


def normalized_stats(document: dict[str, Any]) -> dict[str, Any]:
    normalized = copy.deepcopy(document)
    for run in normalized.get("runs", []):
        for explorer in ("naive", "dpor"):
            run.get(explorer, {}).pop("wall_clock_us", None)
    return normalized


def verify_tree_and_stats(
    showcase_dir: pathlib.Path,
    manifest: dict[str, Any],
) -> int:
    stats_path = showcase_dir / "stats" / "runs.json"
    stats = json.loads(stats_path.read_text())
    if stats.get("provenance") != "MEASURED_ENVIRONMENT_DEPENDENT":
        raise ValueError("stats/runs.json has the wrong provenance")
    stats_by_run: dict[str, dict[str, Any]] = {}
    for run in stats.get("runs", []):
        run_id = run["run_id"]
        if run_id in stats_by_run:
            raise ValueError(f"duplicate stats run: {run_id}")
        stats_by_run[run_id] = run
        for explorer in ("naive", "dpor"):
            wall = run[explorer].get("wall_clock_us")
            if not isinstance(wall, int) or wall < 0:
                raise ValueError(f"invalid wall clock for {run_id}-{explorer}")

    tree_count = 0
    for artifact in manifest["artifacts"]:
        if not artifact["path"].startswith("trees/"):
            continue
        tree_count += 1
        document = json.loads((showcase_dir / artifact["path"]).read_text())
        run_id = artifact["run_id"]
        if run_id not in stats_by_run:
            raise ValueError(f"tree omitted from stats ledger: {run_id}")
        pruning = document["pruning"]
        naive = pruning["naive_equivalent_schedules"]
        dpor = pruning["dpor_schedules_explored"]
        pruned = pruning["schedules_pruned"]
        if dpor + pruned != naive:
            raise ValueError(f"tree schedule conservation failed: {run_id}")
        if document["runs"]["naive"]["schedules_explored"] != naive:
            raise ValueError(f"tree naive count diverged: {run_id}")
        if document["runs"]["dpor"]["schedules_explored"] != dpor:
            raise ValueError(f"tree DPOR count diverged: {run_id}")
        if any(document["runs"][explorer]["exploration_capped"] for explorer in ("naive", "dpor")):
            raise ValueError(f"tree contains a capped run: {run_id}")

        nodes = document["tree"]["nodes"]
        edges = document["tree"]["edges"]
        if [node["id"] for node in nodes] != list(range(len(nodes))):
            raise ValueError(f"tree node ids are not canonical: {run_id}")
        if not nodes or nodes[0]["depth"] != 0 or nodes[0]["status"] != "EXPLORED":
            raise ValueError(f"tree root is invalid: {run_id}")
        if nodes[0]["naive_schedules_below"] != naive or nodes[0]["dpor_schedules_below"] != dpor:
            raise ValueError(f"tree root counts diverged: {run_id}")
        if len(edges) + 1 != len(nodes):
            raise ValueError(f"tree edge/node conservation failed: {run_id}")
        for edge in edges:
            if not (0 <= edge["source"] < edge["target"] < len(nodes)):
                raise ValueError(f"tree edge is not forward/canonical: {run_id}")
            if edge["status"] != nodes[edge["target"]]["status"]:
                raise ValueError(f"tree edge status diverged from target: {run_id}")
        pruned_sum = sum(
            node["naive_schedules_below"]
            for node in nodes
            if node["status"] == "PRUNED"
        )
        if pruned_sum != pruned:
            raise ValueError(f"PRUNED boundaries do not partition schedules: {run_id}")

        stats_run = stats_by_run[run_id]
        for explorer in ("naive", "dpor"):
            if generate.deterministic_run(stats_run[explorer]) != document["runs"][explorer]:
                raise ValueError(f"stats/tree deterministic fields diverged: {run_id}-{explorer}")
        if stats_run["pruning"] != pruning:
            raise ValueError(f"stats/tree pruning fields diverged: {run_id}")

    if tree_count != len(stats_by_run):
        raise ValueError("stats run count differs from tree artifact count")

    for artifact in manifest["artifacts"]:
        if not artifact["path"].endswith("-model-comparison.json"):
            continue
        comparison = json.loads((showcase_dir / artifact["path"]).read_text())
        if comparison["flip"]["from"] == comparison["flip"]["to"]:
            raise ValueError(f"model comparison does not flip: {artifact['path']}")
        if comparison["records"][comparison["baseline_model"]]["property"]["verdict"] != comparison["flip"]["from"]:
            raise ValueError(f"model comparison baseline diverged: {artifact['path']}")
        if comparison["records"][comparison["variant_model"]]["property"]["verdict"] != comparison["flip"]["to"]:
            raise ValueError(f"model comparison variant diverged: {artifact['path']}")
    return tree_count


def endpoint_tuple(step: dict[str, Any]) -> tuple[int, int, int | None]:
    return (step["thread"], step["action_index"], step["flush_address_id"])


def parse_cli_schedule(stdout: str) -> list[tuple[int, int, int | None]]:
    lines = stdout.splitlines()
    try:
        start = lines.index("schedule:") + 1
    except ValueError as error:
        raise ValueError("CLI replay omitted schedule block") from error
    parsed: list[tuple[int, int, int | None]] = []
    for line in lines[start:]:
        words = line.split()
        if not words:
            continue
        if len(words) not in (2, 3):
            raise ValueError(f"invalid CLI schedule line: {line}")
        parsed.append(
            (int(words[0]), int(words[1]), int(words[2]) if len(words) == 3 else None)
        )
    return parsed


def parse_schedule_text(text: str) -> list[tuple[int, int, int | None]]:
    parsed: list[tuple[int, int, int | None]] = []
    for line in text.splitlines():
        words = line.split()
        if len(words) not in (2, 3):
            raise ValueError(f"invalid exported schedule line: {line}")
        parsed.append(
            (int(words[0]), int(words[1]), int(words[2]) if len(words) == 3 else None)
        )
    return parsed


def endpoint_text(step: dict[str, Any]) -> str:
    if step["action_index"] == 2**32 - 1:
        return f"thread {step['thread']} flush"
    return f"thread {step['thread']} action {step['action_index']}"


def verify_cli_report(
    relative: str,
    stdout: str,
    witness: dict[str, Any],
) -> None:
    lines = stdout.splitlines()
    if not lines or not lines[0].startswith("verdict: "):
        raise ValueError(f"{relative}: CLI replay omitted verdict")
    primary = lines[0].split(": ", 1)[1]
    expected_outcome = witness["replay_outcome"]
    if primary != expected_outcome["primary_verdict"]:
        raise ValueError(f"{relative}: CLI primary verdict diverged")
    found = {primary}
    found.update(
        line.split(": ", 1)[1]
        for line in lines
        if line.startswith("also_found: ")
    )
    expected_found = {
        kind for kind, present in expected_outcome["bug_shape"].items() if present
    }
    if found != expected_found:
        raise ValueError(f"{relative}: CLI bug shape diverged")

    reported_witness = witness["reported_primary"]
    expected_reported_schedule = [
        endpoint_tuple(step) for step in reported_witness["schedule"]
    ]
    if parse_cli_schedule(stdout) != expected_reported_schedule:
        raise ValueError(f"{relative}: CLI replay schedule diverged")

    trace_lines: list[str] = []
    in_trace = False
    for line in lines:
        if line == "trace:":
            in_trace = True
            continue
        if line == "schedule:":
            in_trace = False
        elif in_trace and re.match(r"^  \d+: thread \d+ ", line):
            trace_lines.append(line)
    if len(trace_lines) != len(reported_witness["steps"]):
        raise ValueError(f"{relative}: CLI trace length diverged")
    for index, (line, step) in enumerate(zip(trace_lines, reported_witness["steps"])):
        expected_prefix = f"  {index}: thread {step['endpoint']['thread']} "
        if not line.startswith(expected_prefix):
            raise ValueError(f"{relative}: CLI trace endpoint diverged at step {index}")
        label = step["action"]["label"]
        if label not in line and " wait " not in line and " timed_wait " not in line:
            raise ValueError(f"{relative}: CLI action label diverged at step {index}")

    kind = reported_witness["kind"]
    if kind == "race":
        conflict = reported_witness["conflict"]
        required = (
            f"  address: {conflict['address']}",
            f"  first: {endpoint_text(conflict['first'])}",
            f"  second: {endpoint_text(conflict['second'])}",
        )
        if any(line not in lines for line in required):
            raise ValueError(f"{relative}: CLI race identity diverged")
    elif kind == "deadlock":
        for blocked in reported_witness["blocked_threads"]:
            if not any(line.startswith(f"    thread {blocked['thread']}:") for line in lines):
                raise ValueError(f"{relative}: CLI deadlock blocker diverged")
    elif kind == "error":
        if f"  message: {reported_witness['message']}" not in lines:
            raise ValueError(f"{relative}: CLI modeled-error detail diverged")
    elif kind == "assertion":
        if f"  register: r{reported_witness['register']}" not in lines or f"  value: {reported_witness['value']}" not in lines:
            raise ValueError(f"{relative}: CLI assertion detail diverged")
    elif kind == "nontermination":
        if f"  fairness: {reported_witness['fairness']}" not in lines:
            raise ValueError(f"{relative}: CLI lasso fairness diverged")


def verify_replays(
    source_dir: pathlib.Path,
    showcase_dir: pathlib.Path,
    dpor_binary: pathlib.Path,
    manifest: dict[str, Any],
) -> int:
    del source_dir  # The replay deliberately consumes exported curated sources.
    replayed = 0
    for artifact in manifest["artifacts"]:
        relative = artifact["path"]
        if not relative.startswith("schedules/"):
            continue
        replayed += 1
        evidence_path = showcase_dir / artifact["evidence"]
        evidence_document = json.loads(evidence_path.read_text())
        witnesses = [
            witness
            for witness in evidence_document["evidence"]["witnesses"]
            if witness["kind"] == artifact["witness_kind"]
        ]
        if len(witnesses) != 1:
            raise ValueError(f"{relative}: evidence witness is missing or ambiguous")
        expected_input = [endpoint_tuple(step) for step in witnesses[0]["schedule"]]
        try:
            parsed_input = parse_schedule_text((showcase_dir / relative).read_text())
        except (ValueError, UnicodeDecodeError) as error:
            raise ValueError(f"{relative}: exported schedule is invalid: {error}") from error
        if parsed_input != expected_input:
            raise ValueError(f"{relative}: exported schedule diverged from evidence")

        program = showcase_dir / "programs" / f"{artifact['program_id']}.dpor"
        schedule = showcase_dir / relative
        command = [
            str(dpor_binary),
            "replay",
            str(program),
            "--schedule",
            str(schedule),
            "--memory-model",
            artifact["memory_model"],
        ]
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        if completed.returncode != 1:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise ValueError(
                f"{relative}: CLI replay exited {completed.returncode}: {detail}"
            )
        if completed.stderr:
            raise ValueError(f"{relative}: CLI replay wrote stderr")
        verify_cli_report(relative, completed.stdout, witnesses[0])
    return replayed


def run_checked(command: list[str], description: str) -> None:
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{description} failed with {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def ensure_build(source_dir: pathlib.Path, build_dir: pathlib.Path) -> None:
    run_checked(
        ["cmake", "-S", str(source_dir), "-B", str(build_dir)],
        "showcase CMake configure",
    )
    run_checked(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "dpor",
            "dpor_showcase_export",
            "-j8",
        ],
        "showcase binary build",
    )


def verify_regeneration(
    source_dir: pathlib.Path,
    showcase_dir: pathlib.Path,
    exporter: pathlib.Path,
    manifest: dict[str, Any],
) -> int:
    with tempfile.TemporaryDirectory(prefix="dpor-showcase-verify-") as temporary:
        regenerated = pathlib.Path(temporary)
        regenerated_manifest = generate.generate(
            source_dir,
            regenerated,
            exporter,
            source_dir / "showcase" / "config.json",
        )
        if generate.canonical_json_bytes(regenerated_manifest) != (showcase_dir / "manifest.json").read_bytes():
            raise ValueError("manifest.json diverged from regeneration")
        compared = 0
        for artifact in manifest["artifacts"]:
            if not artifact["deterministic"]:
                continue
            compared += 1
            relative = artifact["path"]
            if (showcase_dir / relative).read_bytes() != (regenerated / relative).read_bytes():
                raise ValueError(f"byte divergence from regeneration: {relative}")
        current_stats = json.loads((showcase_dir / "stats" / "runs.json").read_text())
        regenerated_stats = json.loads((regenerated / "stats" / "runs.json").read_text())
        if normalized_stats(current_stats) != normalized_stats(regenerated_stats):
            raise ValueError("stats/runs.json deterministic fields diverged from regeneration")
        return compared


def parse_args() -> argparse.Namespace:
    script_dir = pathlib.Path(__file__).resolve().parent
    source_dir = script_dir.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path, default=source_dir)
    parser.add_argument("--showcase-dir", type=pathlib.Path, default=script_dir)
    parser.add_argument("--build-dir", type=pathlib.Path, default=source_dir / "build")
    parser.add_argument("--skip-build", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    showcase_dir = args.showcase_dir.resolve()
    build_dir = args.build_dir.resolve()
    if not args.skip_build:
        ensure_build(source_dir, build_dir)
        print("showcase verifier: build PASS")

    manifest = json.loads((showcase_dir / "manifest.json").read_text())
    verify_declared_artifacts(source_dir, showcase_dir, manifest)
    tree_count = verify_tree_and_stats(showcase_dir, manifest)
    compared = verify_regeneration(
        source_dir,
        showcase_dir,
        build_dir / "dpor_showcase_export",
        manifest,
    )
    replayed = verify_replays(source_dir, showcase_dir, build_dir / "dpor", manifest)
    summary = manifest["summary"]
    provenance = summary["by_provenance"]
    print(
        "showcase verifier: manifest PASS "
        f"artifacts={summary['artifact_count']} "
        f"curated={provenance['CURATED']} measured={provenance['MEASURED']} "
        f"environment_dependent={provenance['MEASURED_ENVIRONMENT_DEPENDENT']}"
    )
    print(f"showcase verifier: tree conservation PASS runs={tree_count}")
    print(
        "showcase verifier: deterministic regeneration PASS "
        f"artifacts={compared} manifest=byte-identical stats=normalized-identical"
    )
    print(f"showcase verifier: CLI replay PASS schedules={replayed}")
    print("showcase verifier: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
