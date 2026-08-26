#!/usr/bin/env python3
"""Build the deterministic, non-neural model package used by contract tests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
from collections.abc import Callable
from typing import Any

ABI_VERSION = 1
SAMPLE_RATE = 24_000
DEFAULT_SEED = 0x4B696C6978564731
CANONICAL_SPOKEN_SCALARS = 12
CALLBACK_FRAMES = 480

JsonObject = dict[str, Any]
ManifestMutation = Callable[[JsonObject], None]


def _canonical(document: JsonObject) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
            + "\n").encode("utf-8")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _fixture_total_frames(spoken_scalars: int, rate_milli: int) -> int:
    bounded = min(spoken_scalars, 26)
    base_frames = (6 + bounded) * CALLBACK_FRAMES
    return (base_frames * 1000 + rate_milli // 2) // rate_milli


def fixture_smoke_sha256() -> str:
    total_frames = _fixture_total_frames(CANONICAL_SPOKEN_SCALARS, 1000)
    phase = DEFAULT_SEED & 0xFFFFFFFF
    step = (196 << 32) // SAMPLE_RATE
    digest = hashlib.sha256()
    for _ in range(total_frames):
        position = phase >> 16
        if position < 32768:
            triangle = position * 2 - 32768
        else:
            triangle = (65535 - position) * 2 - 32768
        sample = (triangle * 900) // 32768
        # C++ integer division truncates toward zero; Python // rounds down.
        if triangle < 0 and (triangle * 900) % 32768:
            sample += 1
        digest.update(struct.pack("<h", sample))
        phase = (phase + step) & 0xFFFFFFFF
    return digest.hexdigest()


def build_manifest(graph_bytes: bytes) -> JsonObject:
    inventory = b"1\tAA\n2\tAE\n3\tB\n4\tK\n"
    return {
        "audio": {"channels": 1, "sample_format": "s16", "sample_rate": SAMPLE_RATE},
        "determinism": {
            "class": "platform-independent-integer",
            "default_seed": str(DEFAULT_SEED),
            "test_vector_sha256": fixture_smoke_sha256(),
        },
        "engine": {"kind": "fixture-tone/v1"},
        "files": [{
            "bytes": len(graph_bytes),
            "path": "fixture.graph",
            "role": "fixture_graph",
            "sha256": _sha256(graph_bytes),
        }],
        "frontend": {
            "inventory_sha256": _sha256(inventory),
            "schema": "kilix.voicegen.frontend/v1",
            "segment_ids": [1, 2, 3, 4],
            "token_schema": "kilix.voicegen.tokens/v1",
            "unicode_version": "17.0.0",
        },
        "licenses": [{"component": "fixture graph", "license": "First-party test fixture"}],
        "limitations": [
            "Produces a deterministic triangle-wave test signal, not speech.",
            "Contains no neural weights or recorded voice data.",
        ],
        "model_id": "kilix-voicegen-fixture-1",
        "quantization": {"policy": "Integer fixture oscillator; no neural tensors.",
                         "precision": "fixture"},
        "required_cpu_features": [],
        "resources": {"minimum_memory_bytes": 1048576, "model_bytes": len(graph_bytes)},
        "revisions": {
            "architecture": "fixture-tone/v1",
            "export": "first-party-generator/v1",
            "training": "not-applicable",
        },
        "runtime_abi": {"maximum": ABI_VERSION, "minimum": ABI_VERSION},
        "schema": "kilix.voicegen.model/v1",
        "tensors": [
            {"dtype": "int64", "io": "input", "name": "token_ids", "shape": [1, "N"]},
            {"dtype": "int64", "io": "input", "name": "speaker_id", "shape": [1]},
            {"dtype": "float32", "io": "input", "name": "length_scale", "shape": [1]},
            {"dtype": "int16", "io": "output", "name": "audio", "shape": ["T"]},
        ],
        "version": "0.0.0-fixture",
        "voices": [
            {"id": "kilix-female-01", "label": "Kilix Female Fixture"},
            {"id": "kilix-male-01", "label": "Kilix Male Fixture"},
        ],
    }


def _atomic_write(path: pathlib.Path, data: bytes) -> None:
    temporary = path.with_name(path.name + ".part")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def create_fixture_model(output: pathlib.Path, graph_path: pathlib.Path,
                         mutation: ManifestMutation | None = None) -> str:
    if output.is_symlink():
        raise ValueError("fixture output directory must not be a symbolic link")
    output.mkdir(parents=True, exist_ok=True)
    graph_bytes = graph_path.read_bytes()
    manifest = build_manifest(graph_bytes)
    if mutation is not None:
        mutation(manifest)
    manifest_bytes = _canonical(manifest)
    release = {
        "manifest": {
            "bytes": len(manifest_bytes),
            "path": "MANIFEST.json",
            "sha256": _sha256(manifest_bytes),
        },
        "schema": "kilix.voicegen.release/v1",
    }
    release_bytes = _canonical(release)
    release_sha = _sha256(release_bytes)

    _atomic_write(output / "fixture.graph", graph_bytes)
    _atomic_write(output / "MANIFEST.json", manifest_bytes)
    _atomic_write(output / "RELEASE.json", release_bytes)
    _atomic_write(output / "RELEASE.sha256", (release_sha + "\n").encode("ascii"))
    return release_sha


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--graph", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    release_sha = create_fixture_model(arguments.output, arguments.graph)
    print(release_sha)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
