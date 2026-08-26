#!/usr/bin/env python3
"""Optional end-to-end checks for an external research-only Piper package."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import tempfile
import threading
import time
import unittest

from kilix_voicegen import (
    DEFAULT_SEED,
    NativeLibrary,
    Profile,
    SAMPLE_RATE,
    Status,
    VoicegenError,
)


class PiperPilotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = NativeLibrary(os.environ["KGV_LIBRARY"])
        cls.model = pathlib.Path(os.environ["KGV_PIPER_PILOT_MODEL_DIR"])
        cls.release_sha = os.environ["KGV_PIPER_PILOT_RELEASE_SHA"]

    def engine(self):
        return self.native.open_engine(self.model, self.release_sha, threads=6)

    @staticmethod
    def canonical(document: dict[str, object]) -> bytes:
        return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                           sort_keys=True) + "\n").encode("utf-8")

    @staticmethod
    def pcm(engine, text: str) -> bytes:
        blocks = list(engine.stream_pcm(
            text, voice_id="kilix-female-01", profile=Profile.PROSE,
            queue_blocks=4))
        if not blocks:
            raise AssertionError("Piper pilot returned no audio")
        if any(not 0 < len(block) <= 480 * 2 for block in blocks):
            raise AssertionError("Piper callback block violated ABI v1 bounds")
        return b"".join(blocks)

    def test_repeatable_australian_female_speech_path(self) -> None:
        text = "The Australian female pilot can now produce clear synthetic speech."
        with self.engine() as engine:
            first = self.pcm(engine, text)
            second = self.pcm(engine, text)
        self.assertEqual(hashlib.sha256(first).digest(),
                         hashlib.sha256(second).digest())
        self.assertGreater(len(first), SAMPLE_RATE * 2 * 2)
        self.assertLess(len(first), SAMPLE_RATE * 2 * 10)
        self.assertNotEqual(first, bytes(len(first)))

    def test_package_is_female_only_and_seed_is_fixed(self) -> None:
        with self.engine() as engine:
            with self.assertRaises(VoicegenError) as male:
                engine.create_job("hello", voice_id="kilix-male-01",
                                  profile=Profile.PROSE)
            self.assertEqual(male.exception.status, Status.INVALID_VOICE)
            with self.assertRaises(VoicegenError) as seed:
                engine.create_job("hello", voice_id="kilix-female-01",
                                  profile=Profile.PROSE,
                                  seed=DEFAULT_SEED + 1)
            self.assertEqual(seed.exception.status, Status.INVALID_ARGUMENT)

    def test_clean_package_and_rehashed_bad_projection_fails_native_load(self) -> None:
        expected = {
            "MANIFEST.json", "NOTICE.research.txt", "RELEASE.json",
            "RELEASE.sha256", "frontend/lts.jsonl",
            "frontend/piper_projection.jsonl", "frontend/pronunciation.jsonl",
            "frontend/segments.tsv", "frontend/tokens.jsonl", "model.onnx",
        }
        actual = {
            path.relative_to(self.model).as_posix()
            for path in self.model.rglob("*") if path.is_file()
        }
        self.assertEqual(actual, expected)

        with tempfile.TemporaryDirectory(prefix="kgv-piper-projection-") as raw:
            clone = pathlib.Path(raw) / "model"
            shutil.copytree(self.model, clone)
            projection_path = clone / "frontend" / "piper_projection.jsonl"
            documents = [json.loads(line)
                         for line in projection_path.read_text(
                             encoding="utf-8").splitlines()]
            pad = next(document for document in documents[1:]
                       if document["source_id"] == 0)
            pad["target_ids"] = [0]
            projection = b"".join(self.canonical(document)
                                  for document in documents)
            projection_path.write_bytes(projection)

            manifest_path = clone / "MANIFEST.json"
            manifest = json.loads(manifest_path.read_bytes())
            record = next(entry for entry in manifest["files"]
                          if entry["path"] == "frontend/piper_projection.jsonl")
            record["bytes"] = len(projection)
            record["sha256"] = hashlib.sha256(projection).hexdigest()
            manifest["resources"]["model_bytes"] = sum(
                entry["bytes"] for entry in manifest["files"])
            manifest_bytes = self.canonical(manifest)
            manifest_path.write_bytes(manifest_bytes)
            release = {
                "manifest": {
                    "bytes": len(manifest_bytes),
                    "path": "MANIFEST.json",
                    "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                },
                "schema": "kilix.voicegen.release/v1",
            }
            release_bytes = self.canonical(release)
            (clone / "RELEASE.json").write_bytes(release_bytes)
            release_sha = hashlib.sha256(release_bytes).hexdigest()
            (clone / "RELEASE.sha256").write_text(
                release_sha + "\n", encoding="ascii")

            with self.assertRaises(VoicegenError) as caught:
                self.native.open_engine(clone, release_sha, threads=6)
            self.assertEqual(caught.exception.status, Status.INVALID_MODEL)

    def test_cancel_interrupts_onnx_inference(self) -> None:
        text = ("The Australian female technical pilot is producing speech. " * 6).strip()
        with self.engine() as engine:
            job = engine.create_job(text, voice_id="kilix-female-01",
                                    profile=Profile.PROSE)
            result: list[Status] = []

            def run() -> None:
                try:
                    job.run(lambda _block, _rate: True)
                except VoicegenError as error:
                    result.append(error.status)

            thread = threading.Thread(target=run)
            thread.start()
            time.sleep(0.05)
            job.cancel()
            thread.join(10.0)
            self.assertFalse(thread.is_alive())
            self.assertEqual(result, [Status.CANCELLED])
            job.close()


if __name__ == "__main__":
    unittest.main()
