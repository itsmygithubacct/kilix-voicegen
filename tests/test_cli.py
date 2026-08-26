#!/usr/bin/env python3
"""Black-box CLI verification without an audio device or speech artifact."""

from __future__ import annotations

import os
import pathlib
import struct
import subprocess
import tempfile
import unittest


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cli = os.environ["KGV_CLI"]
        cls.model = os.environ["KGV_FIXTURE_DIR"]
        cls.release_sha = os.environ["KGV_FIXTURE_SHA256"]

    def run_cli(self, *arguments: str, input_bytes: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [self.cli, *arguments], input=input_bytes, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=5)

    def test_version_and_verification(self) -> None:
        release_version = self.run_cli("--version")
        self.assertEqual(release_version.returncode, 0, release_version.stderr)
        self.assertEqual(release_version.stdout, b"0.1.0\n")

        version = self.run_cli("--abi-version")
        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertEqual(version.stdout, b"1\n")

        verified = self.run_cli(
            "verify", "--model", self.model, "--release-sha", self.release_sha)
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertIn(b'"status":"ok"', verified.stdout)

        rejected = self.run_cli(
            "verify", "--model", self.model, "--release-sha", "0" * 64)
        self.assertEqual(rejected.returncode, 1)
        self.assertIn(b"KGV_HASH_MISMATCH", rejected.stderr)

    def test_fixture_wav_is_structurally_valid_and_not_overwritten(self) -> None:
        with tempfile.TemporaryDirectory(prefix="kgv-cli-test-") as raw:
            output = pathlib.Path(raw) / "fixture.wav"
            command = (
                "synthesize", "--model", self.model, "--release-sha", self.release_sha,
                "--voice", "kilix-female-01", "--profile", "prose", "--stdin",
                "--output", os.fspath(output),
            )
            result = self.run_cli(*command, input_bytes=b"Kilix fixture")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"24 kHz mono PCM WAV", result.stdout)
            data = output.read_bytes()
            self.assertGreater(len(data), 44)
            self.assertEqual(data[:4], b"RIFF")
            self.assertEqual(data[8:12], b"WAVE")
            self.assertEqual(struct.unpack_from("<I", data, 24)[0], 24_000)
            self.assertEqual(struct.unpack_from("<H", data, 22)[0], 1)
            self.assertEqual(struct.unpack_from("<H", data, 34)[0], 16)
            self.assertEqual(struct.unpack_from("<I", data, 40)[0], len(data) - 44)

            original = data
            refused = self.run_cli(*command, input_bytes=b"different")
            self.assertEqual(refused.returncode, 2)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
