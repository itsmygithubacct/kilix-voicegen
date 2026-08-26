#!/usr/bin/env python3
"""Independently verify a Kilix Voicegen v1 model-package trust chain."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import platform
import re
from typing import Any

SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SAFE_PATH = re.compile(r"[A-Za-z0-9._/-]{1,255}\Z")
ROOT_KEYS = {
    "schema", "model_id", "version", "engine", "revisions", "runtime_abi", "audio",
    "frontend", "voices", "quantization", "required_cpu_features", "files", "tensors",
    "licenses", "determinism", "resources", "limitations",
}
EXPECTED_TENSORS = {
    ("token_ids", "input", "int64", (1, "N")),
    ("speaker_id", "input", "int64", (1,)),
    ("length_scale", "input", "float32", (1,)),
    ("audio", "output", "int16", ("T",)),
}


class VerificationError(RuntimeError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError("JSON contains a duplicate object key")
        result[key] = value
    return result


def _load_json(path: pathlib.Path, limit: int) -> tuple[dict[str, Any], bytes]:
    if path.is_symlink() or not path.is_file():
        raise VerificationError("required metadata is absent or not a regular file")
    data = path.read_bytes()
    if not data or len(data) > limit:
        raise VerificationError("required metadata has an invalid byte count")
    try:
        parsed = json.loads(data, object_pairs_hook=_strict_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise VerificationError("required metadata is not strict UTF-8 JSON") from error
    if not isinstance(parsed, dict):
        raise VerificationError("required metadata root must be an object")
    return parsed, data


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _require_sha(value: Any) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise VerificationError("metadata contains an invalid SHA-256 value")
    return value


def _safe_path(value: Any) -> str:
    if not isinstance(value, str) or SAFE_PATH.fullmatch(value) is None:
        raise VerificationError("payload path is invalid")
    parts = value.split("/")
    if any(part in {"", ".", ".."} for part in parts) or "\\" in value:
        raise VerificationError("payload path is unsafe")
    if value in {"RELEASE.json", "MANIFEST.json", "RELEASE.sha256"}:
        raise VerificationError("metadata file cannot be declared as a payload")
    return value


def _check_no_symlink(root: pathlib.Path, relative: str) -> pathlib.Path:
    current = root
    for part in relative.split("/"):
        current = current / part
        if current.is_symlink():
            raise VerificationError("model payload must not traverse a symbolic link")
    if not current.is_file():
        raise VerificationError("required payload is absent or not a regular file")
    return current


def verify_model(directory: pathlib.Path, expected_release_sha256: str) -> dict[str, Any]:
    _require_sha(expected_release_sha256)
    if directory.is_symlink() or not directory.is_dir():
        raise VerificationError("model directory is absent or a symbolic link")

    release, release_bytes = _load_json(directory / "RELEASE.json", 65_536)
    if _hash(release_bytes) != expected_release_sha256:
        raise VerificationError("RELEASE.json does not match the caller-pinned SHA-256")
    if set(release) != {"schema", "manifest"} or release["schema"] != "kilix.voicegen.release/v1":
        raise VerificationError("release schema or fields are unsupported")
    reference = release["manifest"]
    if not isinstance(reference, dict) or set(reference) != {"path", "bytes", "sha256"}:
        raise VerificationError("manifest reference fields are invalid")
    if reference["path"] != "MANIFEST.json" or not isinstance(reference["bytes"], int):
        raise VerificationError("manifest reference is invalid")

    manifest, manifest_bytes = _load_json(directory / "MANIFEST.json", 1_048_576)
    if len(manifest_bytes) != reference["bytes"] or _hash(manifest_bytes) != _require_sha(reference["sha256"]):
        raise VerificationError("MANIFEST.json does not match RELEASE.json")
    if set(manifest) != ROOT_KEYS or manifest["schema"] != "kilix.voicegen.model/v1":
        raise VerificationError("model manifest schema or fields are unsupported")
    if manifest["engine"] != {"kind": "fixture-tone/v1"}:
        raise VerificationError("model engine kind is unsupported")
    runtime_abi = manifest["runtime_abi"]
    if (not isinstance(runtime_abi, dict) or set(runtime_abi) != {"minimum", "maximum"}
            or not isinstance(runtime_abi["minimum"], int)
            or not isinstance(runtime_abi["maximum"], int)
            or not runtime_abi["minimum"] <= 1 <= runtime_abi["maximum"]):
        raise VerificationError("model runtime ABI is incompatible")
    if manifest["audio"] != {"channels": 1, "sample_format": "s16", "sample_rate": 24000}:
        raise VerificationError("model audio contract is unsupported")

    frontend = manifest["frontend"]
    if (not isinstance(frontend, dict)
            or set(frontend) != {"schema", "dialect", "unicode_version", "token_schema",
                                "inventory_sha256", "segment_ids"}
            or frontend.get("schema") != "kilix.voicegen.frontend/v1"
            or frontend.get("dialect") != "en-AU"
            or frontend.get("token_schema") != "kilix.voicegen.tokens/v1"
            or frontend.get("unicode_version") != "17.0.0"
            or frontend.get("segment_ids") != [1, 2, 3, 4]):
        raise VerificationError("frontend contract is unsupported")
    _require_sha(frontend.get("inventory_sha256"))

    voices = manifest["voices"]
    if (not isinstance(voices, list) or len(voices) != 2
            or {voice.get("id") for voice in voices if isinstance(voice, dict)}
            != {"kilix-female-01", "kilix-male-01"}):
        raise VerificationError("model must contain exactly the two v1 voice IDs")

    features = manifest["required_cpu_features"]
    if not isinstance(features, list) or any(not isinstance(item, str) for item in features):
        raise VerificationError("CPU feature list is invalid")
    supported = {"sse2"} if platform.machine().lower() in {"x86_64", "amd64"} else set()
    if set(features) - supported:
        raise VerificationError("model requires an unsupported CPU feature")

    tensors = manifest["tensors"]
    try:
        actual_tensors = {
            (item["name"], item["io"], item["dtype"], tuple(item["shape"]))
            for item in tensors
            if isinstance(item, dict) and set(item) == {"name", "io", "dtype", "shape"}
        }
    except (KeyError, TypeError) as error:
        raise VerificationError("tensor metadata is invalid") from error
    if len(tensors) != len(actual_tensors) or actual_tensors != EXPECTED_TENSORS:
        raise VerificationError("fixture tensor contract is unknown or incomplete")

    files = manifest["files"]
    if not isinstance(files, list) or not 1 <= len(files) <= 256:
        raise VerificationError("payload file list is invalid")
    paths: set[str] = set()
    total_bytes = 0
    fixture_graph = False
    for item in files:
        if not isinstance(item, dict) or set(item) != {"path", "role", "bytes", "sha256"}:
            raise VerificationError("payload metadata fields are invalid")
        relative = _safe_path(item["path"])
        if relative in paths:
            raise VerificationError("payload path is duplicated")
        paths.add(relative)
        payload_path = _check_no_symlink(directory, relative)
        payload = payload_path.read_bytes()
        if (not isinstance(item["bytes"], int) or len(payload) != item["bytes"]
                or _hash(payload) != _require_sha(item["sha256"])):
            raise VerificationError("payload does not match its byte count or SHA-256")
        total_bytes += len(payload)
        fixture_graph |= relative == "fixture.graph" and item["role"] == "fixture_graph"
    if not fixture_graph:
        raise VerificationError("deterministic fixture graph is absent")
    resources = manifest["resources"]
    if not isinstance(resources, dict) or resources.get("model_bytes") != total_bytes:
        raise VerificationError("model resource byte total is invalid")

    determinism = manifest["determinism"]
    if not isinstance(determinism, dict):
        raise VerificationError("determinism metadata is invalid")
    _require_sha(determinism.get("test_vector_sha256"))
    return {
        "schema": "kilix.voicegen.verification/v1",
        "status": "ok",
        "model_id": manifest["model_id"],
        "version": manifest["version"],
        "release_sha256": expected_release_sha256,
        "payload_files": len(files),
        "payload_bytes": total_bytes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=pathlib.Path, required=True)
    parser.add_argument("--release-sha", required=True)
    arguments = parser.parse_args()
    try:
        result = verify_model(arguments.model, arguments.release_sha)
    except VerificationError as error:
        print(json.dumps({"schema": "kilix.voicegen.verification/v1",
                          "status": "error", "detail": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
