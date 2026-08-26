#!/usr/bin/env python3
"""Execute the frozen P0 slice and inventory the complete Phase 2 vector set."""

from __future__ import annotations

import json
import os
import pathlib
import unittest
from typing import Any

from kilix_voicegen import NativeLibrary, Profile, Status, VoicegenError

VECTORS = pathlib.Path(__file__).resolve().parent / "conformance" / "frontend_v1.jsonl"


def materialize(specification: dict[str, Any]) -> bytes:
    if "utf8" in specification:
        return specification["utf8"].encode("utf-8")
    if "hex" in specification:
        return bytes.fromhex(specification["hex"])
    return specification["repeat"].encode("utf-8") * specification["count"]


class FrontendConformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = NativeLibrary(os.environ["KGV_LIBRARY"])
        cls.engine = cls.native.open_engine(
            os.environ["KGV_FIXTURE_DIR"], os.environ["KGV_FIXTURE_SHA256"])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.engine.close()

    def test_vectors_are_unique_versioned_and_p0_is_executable(self) -> None:
        records: list[dict[str, Any]] = []
        with VECTORS.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    self.fail(f"vector line {line_number} is not JSON: {error}")
                records.append(record)

        identifiers = [record["id"] for record in records]
        self.assertEqual(len(identifiers), len(set(identifiers)))
        self.assertTrue(all(record["schema"] ==
                            "kilix.voicegen.frontend-conformance-case/v1"
                            for record in records))
        self.assertTrue(all(record["dialect"] == "en-AU" for record in records))
        p0 = [record for record in records if record["stage"] == "p0-unicode-control"]
        planned = [record for record in records if record["stage"] != "p0-unicode-control"]
        self.assertGreaterEqual(len(p0), 25)
        self.assertGreaterEqual(len(planned), 15)

        for record in p0:
            with self.subTest(vector=record["id"]):
                expected_name = record["expect"]["status"]
                expected_status = Status[expected_name.removeprefix("KGV_")]
                profile = Profile.PROSE if record["profile"] == "prose" else Profile.TERMINAL
                audio = False
                try:
                    job = self.engine.create_job(
                        materialize(record["input"]), voice_id="kilix-female-01",
                        profile=profile)
                    try:
                        def accept(block: bytes, _sample_rate: int) -> bool:
                            nonlocal audio
                            audio |= bool(block)
                            return True

                        job.run(accept)
                        actual_status = Status.OK
                    finally:
                        job.close()
                except VoicegenError as error:
                    actual_status = error.status
                self.assertEqual(actual_status, expected_status)
                if expected_status is Status.OK:
                    self.assertEqual(audio, record["expect"]["audio"])


if __name__ == "__main__":
    unittest.main()
