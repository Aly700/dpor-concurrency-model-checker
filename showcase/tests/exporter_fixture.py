#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: exporter_fixture.py EXPORTER SOURCE_DIR")
    exporter = pathlib.Path(sys.argv[1]).resolve()
    source_dir = pathlib.Path(sys.argv[2]).resolve()
    command = [
        str(exporter),
        "--program",
        str(source_dir / "examples" / "data_race.dpor"),
        "--program-id",
        "data_race",
        "--memory-model",
        "sc",
        "--step-bound",
        "4",
        "--max-schedules",
        "100000",
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise AssertionError(
            f"exporter failed with {completed.returncode}: {completed.stderr}"
        )

    exported = json.loads(completed.stdout)
    assert exported["schema_version"] == 1
    assert exported["program_id"] == "data_race"
    assert exported["memory_model"] == "sc"
    assert exported["runs"]["naive"]["schedules_explored"] == 2
    assert exported["runs"]["dpor"]["schedules_explored"] == 2
    assert exported["pruning"]["naive_equivalent_schedules"] == 2
    assert exported["pruning"]["schedules_pruned"] == 0

    root = exported["tree"]["nodes"][0]
    assert root["id"] == 0
    assert root["status"] == "EXPLORED"
    assert root["enabled_threads"] == [0, 1]

    race = next(
        witness
        for witness in exported["evidence"]["witnesses"]
        if witness["kind"] == "race"
    )
    assert race["conflict"]["address"] == "x"
    assert race["conflict"]["first_clock"] == [1, 0]
    assert race["conflict"]["second_clock"] == [0, 1]
    assert completed.stdout.endswith("\n")
    print("exporter_fixture: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
