#!/usr/bin/env python3
"""Execute the frozen Unicode/control and lexical frontend vector set."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
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
        cls.cli = os.environ["KGV_CLI"]
        cls.engine = cls.native.open_engine(
            os.environ["KGV_FIXTURE_DIR"], os.environ["KGV_FIXTURE_SHA256"])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.engine.close()

    def test_vectors_are_unique_versioned_and_executable(self) -> None:
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

        for record in planned:
            with self.subTest(vector=record["id"]):
                profile = record["profile"]
                completed = subprocess.run(
                    [self.cli, "frontend", "--profile", profile, "--stdin"],
                    input=materialize(record["input"]), stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, check=False, timeout=5)
                self.assertIn(completed.returncode, (0, 1), completed.stderr)
                trace = json.loads(completed.stdout)
                self.assertEqual(trace["schema"],
                                 "kilix.voicegen.frontend-lexical-trace/v1")
                self.assertEqual(trace["status"], record["expect"]["status"])
                if "words" in record["expect"]:
                    self.assertEqual(
                        [word["normalized"] for word in trace["words"]],
                        record["expect"]["words"])
                if "word_spans" in record["expect"]:
                    self.assertEqual(
                        [[word["span"]["byte_start"], word["span"]["byte_end"]]
                         for word in trace["words"]],
                        record["expect"]["word_spans"])
                if "diagnostics" in record["expect"]:
                    actual = [diagnostic["code"] for diagnostic in trace["diagnostics"]]
                    for expected in record["expect"]["diagnostics"]:
                        self.assertIn(expected, actual)
                if "diagnostic_spans" in record["expect"]:
                    self.assertEqual(
                        [{"code": diagnostic["code"],
                          "span": [diagnostic["span"]["byte_start"],
                                   diagnostic["span"]["byte_end"]]}
                         for diagnostic in trace["diagnostics"]],
                        record["expect"]["diagnostic_spans"])
                if "ignored_control_sequences" in record["expect"]:
                    self.assertEqual(trace["ignored_control_sequences"],
                                     record["expect"]["ignored_control_sequences"])
                if "terminator" in record["expect"]:
                    self.assertIn(
                        record["expect"]["terminator"],
                        [phrase["terminator"] for phrase in trace["phrases"]])
                if "terminators" in record["expect"]:
                    self.assertEqual(
                        [phrase["terminator"] for phrase in trace["phrases"]],
                        record["expect"]["terminators"])

    def test_frontend_is_locale_and_timezone_independent(self) -> None:
        cases = [
            ("prose", "08/09/2010 and $1.05"),
            ("prose", "cafe\u0301 — £2"),
            ("terminal", "$HOME 127.0.0.1:8080 README.md"),
        ]
        environments = [
            ("C", "UTC"),
            ("C.UTF-8", "Australia/Sydney"),
        ]
        for profile, source in cases:
            outputs: list[bytes] = []
            for locale, timezone in environments:
                environment = os.environ.copy()
                environment["LC_ALL"] = locale
                environment["LANG"] = locale
                environment["TZ"] = timezone
                completed = subprocess.run(
                    [self.cli, "frontend", "--profile", profile, "--stdin"],
                    input=source.encode("utf-8"), stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, check=False, timeout=5,
                    env=environment)
                self.assertIn(completed.returncode, (0, 1), completed.stderr)
                outputs.append(completed.stdout)
            self.assertEqual(outputs[0], outputs[1], source)


if __name__ == "__main__":
    unittest.main()
