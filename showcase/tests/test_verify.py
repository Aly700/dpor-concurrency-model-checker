#!/usr/bin/env python3
import hashlib
import json
import pathlib
import shutil
import tempfile
import unittest

from showcase import verify


SOURCE_DIR = pathlib.Path(__file__).resolve().parents[2]
SHOWCASE_DIR = SOURCE_DIR / "showcase"


class VerifierTamperTests(unittest.TestCase):
    def copied_showcase(self, temporary: str) -> pathlib.Path:
        destination = pathlib.Path(temporary) / "showcase"
        shutil.copytree(SHOWCASE_DIR, destination)
        return destination

    def test_names_tampered_measured_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = self.copied_showcase(temporary)
            target = copied / "trees" / "data_race-sc.json"
            target.write_bytes(target.read_bytes() + b" ")
            manifest = json.loads((copied / "manifest.json").read_text())
            with self.assertRaisesRegex(ValueError, "trees/data_race-sc.json"):
                verify.verify_declared_artifacts(SOURCE_DIR, copied, manifest)

    def test_names_tampered_curated_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = self.copied_showcase(temporary)
            target = copied / "programs" / "data_race.dpor"
            target.write_bytes(target.read_bytes() + b"# tampered\n")
            manifest = json.loads((copied / "manifest.json").read_text())
            with self.assertRaisesRegex(ValueError, "programs/data_race.dpor"):
                verify.verify_declared_artifacts(SOURCE_DIR, copied, manifest)

    def test_replay_gate_rejects_tampered_schedule_endpoint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = self.copied_showcase(temporary)
            schedule = copied / "schedules" / "data_race-sc-race.schedule"
            schedule.write_text("99 0\n1 0\n")
            manifest_path = copied / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            for artifact in manifest["artifacts"]:
                if artifact["path"] == "schedules/data_race-sc-race.schedule":
                    data = schedule.read_bytes()
                    artifact["bytes"] = len(data)
                    artifact["sha256"] = hashlib.sha256(data).hexdigest()
                    break
            with self.assertRaisesRegex(ValueError, "data_race-sc-race.schedule"):
                verify.verify_replays(
                    SOURCE_DIR,
                    copied,
                    SOURCE_DIR / "build" / "dpor",
                    manifest,
                )

    def test_rejects_invalid_manifest_label(self) -> None:
        manifest = json.loads((SHOWCASE_DIR / "manifest.json").read_text())
        manifest["artifacts"][0]["provenance"] = "GUESSED"
        with self.assertRaisesRegex(ValueError, "invalid provenance"):
            verify.validate_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
