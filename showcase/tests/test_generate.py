#!/usr/bin/env python3
import json
import pathlib
import tempfile
import unittest

from showcase import generate


class GeneratorValidationTests(unittest.TestCase):
    def test_rejects_duplicate_program_ids(self) -> None:
        config = {
            "programs": [
                {"id": "same", "source": "a.dpor", "runs": []},
                {"id": "same", "source": "b.dpor", "runs": []},
            ]
        }
        with self.assertRaisesRegex(ValueError, "duplicate program id"):
            generate.validate_config(config, pathlib.Path("."))

    def test_rejects_missing_curated_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = {
                "programs": [
                    {"id": "missing", "source": "missing.dpor", "runs": []}
                ]
            }
            with self.assertRaisesRegex(ValueError, "missing curated source"):
                generate.validate_config(config, pathlib.Path(temporary))

    def test_rejects_capped_export(self) -> None:
        exported = {
            "runs": {
                "naive": {"exploration_capped": False, "schedules_explored": 2},
                "dpor": {"exploration_capped": True, "schedules_explored": 1},
            },
            "pruning": {
                "naive_equivalent_schedules": 2,
                "dpor_schedules_explored": 1,
                "schedules_pruned": 1,
            },
        }
        with self.assertRaisesRegex(ValueError, "capped"):
            generate.validate_export(exported, "fixture-sc")

    def test_rejects_declared_property_flip_without_a_flip(self) -> None:
        records = {
            "sc": {"property": {"verdict": "holds"}},
            "pso": {"property": {"verdict": "holds"}},
        }
        with self.assertRaisesRegex(ValueError, "does not flip"):
            generate.build_model_comparison("fixture", records, "sc", "pso")

    def test_rejects_deterministic_manifest_entry_without_sha256(self) -> None:
        manifest = {
            "artifacts": [
                {
                    "path": "trees/fixture.json",
                    "provenance": "MEASURED",
                    "deterministic": True,
                    "bytes": 10,
                    "sha256": None,
                }
            ]
        }
        with self.assertRaisesRegex(ValueError, "missing sha256"):
            generate.validate_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
