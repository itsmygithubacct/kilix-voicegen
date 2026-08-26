#!/usr/bin/env python3
"""Machine-check repository boundaries that should hold before every push."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import unittest
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
BANNED_SUFFIXES = {".wav", ".flac", ".onnx", ".pt", ".pth", ".safetensors", ".npy", ".npz"}


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


class RepositoryBoundaryTests(unittest.TestCase):
    def test_json_documents_are_strict_and_versioned(self) -> None:
        paths = sorted((ROOT / "schemas").glob("*.json"))
        paths += [ROOT / "cmake" / "dependencies.lock.json",
                  ROOT / "benchmarks" / "baseline.lock.json",
                  ROOT / "tests" / "fixtures" / "fixtures.lock.json"]
        self.assertGreaterEqual(len(paths), 8)
        for path in paths:
            with self.subTest(path=path.name), path.open(encoding="utf-8") as stream:
                document = json.load(stream, object_pairs_hook=strict_object)
                self.assertIsInstance(document, dict)

    def test_fixture_lock_matches_first_party_files(self) -> None:
        lock = json.loads((ROOT / "tests" / "fixtures" / "fixtures.lock.json").read_text(),
                          object_pairs_hook=strict_object)
        self.assertEqual(lock["schema"], "kilix.voicegen.test-fixtures/v1")
        for fixture in lock["fixtures"]:
            path = ROOT / "tests" / "fixtures" / fixture["path"]
            data = path.read_bytes()
            self.assertEqual(len(data), fixture["bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), fixture["sha256"])
            self.assertEqual(fixture["origin"], "first-party")
            self.assertTrue(fixture["license"])
            self.assertTrue(fixture["purpose"])

    def test_benchmark_lock_is_hash_only_and_well_formed(self) -> None:
        lock = json.loads((ROOT / "benchmarks" / "baseline.lock.json").read_text(),
                          object_pairs_hook=strict_object)
        self.assertEqual(lock["schema"], "kilix.voicegen.benchmark-inputs/v1")
        self.assertGreaterEqual(len(lock["inputs"]), 4)
        for entry in lock["inputs"]:
            self.assertFalse(entry["source_name"].startswith("/"))
            self.assertNotIn("..", entry["source_name"].split("/"))
            self.assertIsNotNone(SHA256.fullmatch(entry["sha256"]))
            self.assertGreater(entry["bytes"], 0)

    def test_no_model_or_audio_artifact_is_present(self) -> None:
        offenders: list[str] = []
        for path in ROOT.rglob("*"):
            if ".git" in path.parts or not path.is_file():
                continue
            if path.suffix.lower() in BANNED_SUFFIXES:
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_no_generated_python_cache_is_present(self) -> None:
        offenders = [
            path.relative_to(ROOT).as_posix()
            for path in ROOT.rglob("*")
            if ".git" not in path.parts
            and (path.name == "__pycache__" or path.suffix in {".pyc", ".pyo"})
        ]
        self.assertEqual(offenders, [])

    def test_no_absolute_user_or_research_path_is_embedded(self) -> None:
        offenders: list[str] = []
        banned = ("/" + "home" + "/", "~/" + "research" + "/",
                  "gpu_" + "terminal/kilix-apps")
        for path in ROOT.rglob("*"):
            if ".git" in path.parts or not path.is_file() or path.suffix in {".so", ".o", ".a"}:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if any(pattern in text for pattern in banned):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
