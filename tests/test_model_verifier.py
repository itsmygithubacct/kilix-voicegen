#!/usr/bin/env python3
"""Corruption and compatibility matrix for both independent package verifiers."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
from collections.abc import Callable
from typing import Any

from kilix_voicegen import NativeLibrary, Profile, Status, VoicegenError

ROOT = pathlib.Path(__file__).resolve().parent.parent


def _load_tool(name: str):
    path = ROOT / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"kgv_test_{name}", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path.name}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = _load_tool("make_fixture_model")
VERIFIER = _load_tool("verify_model")
Mutation = Callable[[dict[str, Any]], None]


def canonical(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


class ModelVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = NativeLibrary(os.environ["KGV_LIBRARY"])
        cls.graph = pathlib.Path(os.environ["KGV_FIXTURE_GRAPH"])

    def make_model(self, mutation: Mutation | None = None) -> tuple[pathlib.Path, str, tempfile.TemporaryDirectory[str]]:
        temporary = tempfile.TemporaryDirectory(prefix="kgv-model-test-")
        directory = pathlib.Path(temporary.name) / "model"
        release_sha = GENERATOR.create_fixture_model(directory, self.graph, mutation)
        return directory, release_sha, temporary

    def assert_native_status(self, directory: pathlib.Path, release_sha: str,
                             expected: Status) -> None:
        if expected is Status.OK:
            with self.native.open_engine(directory, release_sha):
                return
        with self.assertRaises(VoicegenError) as caught:
            self.native.open_engine(directory, release_sha)
        self.assertEqual(caught.exception.status, expected)

    def replace_payload(self, directory: pathlib.Path, relative: str,
                        payload: bytes) -> str:
        (directory / relative).write_bytes(payload)
        manifest_path = directory / "MANIFEST.json"
        manifest = json.loads(manifest_path.read_bytes())
        for record in manifest["files"]:
            if record["path"] == relative:
                record["bytes"] = len(payload)
                record["sha256"] = hashlib.sha256(payload).hexdigest()
                break
        else:
            self.fail(f"payload {relative!r} is not declared")
        manifest["resources"]["model_bytes"] = sum(
            record["bytes"] for record in manifest["files"])
        manifest_bytes = canonical(manifest)
        manifest_path.write_bytes(manifest_bytes)
        release = {
            "manifest": {
                "bytes": len(manifest_bytes),
                "path": "MANIFEST.json",
                "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            },
            "schema": "kilix.voicegen.release/v1",
        }
        release_bytes = canonical(release)
        (directory / "RELEASE.json").write_bytes(release_bytes)
        release_sha = hashlib.sha256(release_bytes).hexdigest()
        (directory / "RELEASE.sha256").write_text(release_sha + "\n", encoding="ascii")
        return release_sha

    def test_valid_package_passes_both_verifiers(self) -> None:
        directory, release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        result = VERIFIER.verify_model(directory, release_sha)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["payload_files"], 5)
        self.assert_native_status(directory, release_sha, Status.OK)

    def test_wrong_outer_hash_fails_before_metadata(self) -> None:
        directory, _release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, "0" * 64, Status.HASH_MISMATCH)

    def test_unknown_model_schema_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["schema"] = "kilix.voicegen.model/v999"

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha, Status.UNSUPPORTED_SCHEMA)

    def test_incompatible_runtime_abi_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["runtime_abi"] = {"minimum": 2, "maximum": 2}

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha, Status.ABI_MISMATCH)

    def test_non_australian_frontend_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["frontend"]["dialect"] = "en-US"

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        with self.assertRaises(VERIFIER.VerificationError):
            VERIFIER.verify_model(directory, release_sha)
        self.assert_native_status(directory, release_sha, Status.UNSUPPORTED_SCHEMA)

    def test_unknown_cpu_requirement_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["required_cpu_features"] = ["future-vector-999"]

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha, Status.UNSUPPORTED_CPU)

    def test_unknown_tensor_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["tensors"][0]["name"] = "guessed_input"

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha, Status.INVALID_MODEL)

    def test_bad_smoke_vector_fails_closed(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["determinism"]["test_vector_sha256"] = "f" * 64

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha, Status.INVALID_MODEL)

    def test_frontend_abi_binding_is_loaded_not_only_declared(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["frontend"]["abi_sha256"] = "f" * 64

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assertEqual(VERIFIER.verify_model(directory, release_sha)["status"], "ok")
        self.assert_native_status(directory, release_sha, Status.ABI_MISMATCH)

    def test_missing_or_duplicate_frontend_role_fails_closed(self) -> None:
        def missing(manifest: dict[str, Any]) -> None:
            next(record for record in manifest["files"]
                 if record["role"] == "lts_model")["role"] = "unknown_resource"

        directory, release_sha, temporary = self.make_model(missing)
        self.addCleanup(temporary.cleanup)
        with self.assertRaises(VERIFIER.VerificationError):
            VERIFIER.verify_model(directory, release_sha)
        self.assert_native_status(directory, release_sha, Status.INVALID_MODEL)

        def duplicate(manifest: dict[str, Any]) -> None:
            next(record for record in manifest["files"]
                 if record["role"] == "lts_model")["role"] = "pronunciation_lexicon"

        directory2, release_sha2, temporary2 = self.make_model(duplicate)
        self.addCleanup(temporary2.cleanup)
        with self.assertRaises(VERIFIER.VerificationError):
            VERIFIER.verify_model(directory2, release_sha2)
        self.assert_native_status(directory2, release_sha2, Status.INVALID_MODEL)

    def test_rehashed_malformed_frontend_resource_fails_native_load(self) -> None:
        directory, _release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        lexicon_path = directory / "frontend" / "pronunciation.jsonl"
        malformed = b"[" + lexicon_path.read_bytes()[1:]
        release_sha = self.replace_payload(
            directory, "frontend/pronunciation.jsonl", malformed)
        self.assertEqual(VERIFIER.verify_model(directory, release_sha)["status"], "ok")
        self.assert_native_status(directory, release_sha, Status.INVALID_MODEL)

    def test_single_fixed_voice_package_is_supported(self) -> None:
        def mutate(manifest: dict[str, Any]) -> None:
            manifest["voices"] = [manifest["voices"][0]]

        directory, release_sha, temporary = self.make_model(mutate)
        self.addCleanup(temporary.cleanup)
        self.assertEqual(VERIFIER.verify_model(directory, release_sha)["status"], "ok")
        with self.native.open_engine(directory, release_sha) as engine:
            with self.assertRaises(VoicegenError) as caught:
                engine.create_job("hello", voice_id="kilix-male-01",
                                  profile=Profile.PROSE)
            self.assertEqual(caught.exception.status, Status.INVALID_VOICE)

    def test_corrupt_or_missing_payload_has_distinct_status(self) -> None:
        directory, release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        (directory / "fixture.graph").write_bytes(b"corrupt")
        self.assert_native_status(directory, release_sha, Status.HASH_MISMATCH)

        directory2, release_sha2, temporary2 = self.make_model()
        self.addCleanup(temporary2.cleanup)
        (directory2 / "fixture.graph").unlink()
        self.assert_native_status(directory2, release_sha2, Status.IO_ERROR)

    def test_payload_symlink_is_never_followed(self) -> None:
        directory, release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        payload = directory / "fixture.graph"
        payload.unlink()
        payload.symlink_to(self.graph)
        self.assert_native_status(directory, release_sha, Status.INVALID_MODEL)

    def test_unknown_release_schema_fails_closed(self) -> None:
        directory, _release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        release_path = directory / "RELEASE.json"
        release = json.loads(release_path.read_text(encoding="utf-8"))
        release["schema"] = "kilix.voicegen.release/v999"
        data = (json.dumps(release, separators=(",", ":"), sort_keys=True) + "\n").encode()
        release_path.write_bytes(data)
        release_sha = hashlib.sha256(data).hexdigest()
        self.assert_native_status(directory, release_sha, Status.UNSUPPORTED_SCHEMA)

    def test_uppercase_expected_hash_is_invalid_argument(self) -> None:
        directory, release_sha, temporary = self.make_model()
        self.addCleanup(temporary.cleanup)
        self.assert_native_status(directory, release_sha.upper(), Status.INVALID_ARGUMENT)


if __name__ == "__main__":
    unittest.main()
