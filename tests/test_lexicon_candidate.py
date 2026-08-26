#!/usr/bin/env python3
"""Black-box tests for the deterministic en-AU lexicon candidate builder."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import unittest


class LexiconCandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = os.environ["KGV_LEXICON_TOOL"]

    @staticmethod
    def sha(data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    def invoke(self, root: pathlib.Path, dictionary: bytes, *, name: str,
               expected_sha: str | None = None) -> subprocess.CompletedProcess[bytes]:
        dictionary_path = root / f"{name}.dict"
        license_path = root / f"{name}.license"
        output = root / f"{name}.output"
        dictionary_path.write_bytes(dictionary)
        license_data = b"First-party synthetic test license.\n"
        license_path.write_bytes(license_data)
        return subprocess.run([
            self.tool,
            "--input", os.fspath(dictionary_path),
            "--input-sha256", expected_sha or self.sha(dictionary),
            "--source-revision", "1" * 40,
            "--generator-revision", "2" * 40,
            "--source-license", os.fspath(license_path),
            "--source-license-sha256", self.sha(license_data),
            "--output-dir", os.fspath(output),
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=10)

    def test_candidate_is_deterministic_and_preserves_review_flags(self) -> None:
        dictionary = (
            b"about AH0 B AW1 T\n"
            b"aalborg AO1 L B AO0 R G # place, danish\n"
            b"car K AA1 R\n"
            b"carry K AE1 R IY0\n"
            b"hm HH M\n"
            b"water W AO1 T ER0\n"
        )
        with tempfile.TemporaryDirectory(prefix="kgv-lexicon-test-") as raw:
            root = pathlib.Path(raw)
            first = self.invoke(root, dictionary, name="first")
            second = self.invoke(root, dictionary, name="second")
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            first_dir = root / "first.output"
            second_dir = root / "second.output"
            for name in ("CMUDICT_LICENSE", "POLICY.json", "candidate.jsonl",
                         "review-queue.jsonl", "MANIFEST.json"):
                self.assertEqual((first_dir / name).read_bytes(),
                                 (second_dir / name).read_bytes())

            records = [json.loads(line) for line in
                       (first_dir / "candidate.jsonl").read_text().splitlines()]
            by_word = {record["grapheme"]: record for record in records}
            self.assertEqual(by_word["about"]["candidate_segments"],
                             ["AX", "B", "AW", "T"])
            self.assertEqual(by_word["car"]["candidate_segments"],
                             ["K", "AA_LOT_PALM_REVIEW"])
            self.assertIn("POSTVOCALIC_R_DROPPED", by_word["car"]["review_flags"])
            self.assertIn("AE_TRAP_BATH_SPLIT_REVIEW", by_word["carry"]["review_flags"])
            self.assertIn("R", by_word["carry"]["candidate_segments"])
            self.assertEqual(by_word["hm"]["candidate_segments"], ["HH", "M"])
            self.assertEqual(by_word["hm"]["stresses"], [-1, -1])
            self.assertIn("NO_PRONOUNCED_VOWEL_REVIEW", by_word["hm"]["review_flags"])
            self.assertEqual(by_word["water"]["candidate_segments"],
                             ["W", "AO_LONG", "T", "AX"])
            self.assertIn("RHOTIC_VOWEL_TRANSFORMED", by_word["water"]["review_flags"])

            manifest = json.loads((first_dir / "MANIFEST.json").read_text())
            self.assertEqual(manifest["candidate_entries"], 6)
            self.assertEqual(manifest["review_queue_entries"], 5)
            self.assertEqual(manifest["generator"]["revision"], "2" * 40)
            self.assertEqual(
                manifest["generator"]["sha256"],
                self.sha(pathlib.Path(self.tool).resolve().read_bytes()))
            self.assertEqual(manifest["status"],
                             "machine-transformed-unreviewed-not-product-admitted")

    def test_hash_mismatch_and_malformed_phone_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="kgv-lexicon-test-") as raw:
            root = pathlib.Path(raw)
            mismatch = self.invoke(root, b"car K AA1 R\n", name="mismatch",
                                   expected_sha="0" * 64)
            self.assertEqual(mismatch.returncode, 1)
            self.assertIn(b"SHA-256 mismatch", mismatch.stderr)
            self.assertFalse((root / "mismatch.output").exists())

            malformed = self.invoke(root, b"word W UNKNOWN1 D\n", name="malformed")
            self.assertEqual(malformed.returncode, 1)
            self.assertIn(b"unknown or malformed CMU phone", malformed.stderr)
            self.assertFalse((root / "malformed.output").exists())

    def test_existing_output_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory(prefix="kgv-lexicon-test-") as raw:
            root = pathlib.Path(raw)
            dictionary = b"word W ER1 D\n"
            first = self.invoke(root, dictionary, name="same")
            self.assertEqual(first.returncode, 0, first.stderr)
            marker = root / "same.output" / "marker"
            marker.write_text("retain")
            second = self.invoke(root, dictionary, name="same")
            self.assertEqual(second.returncode, 1)
            self.assertEqual(marker.read_text(), "retain")


if __name__ == "__main__":
    unittest.main()
