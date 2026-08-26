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

CONTROLS = (
    "PAD", "BOS", "EOS", "WB", "SYL", "STRESS_0", "STRESS_1",
    "STRESS_2", "END_NONE", "END_COMMA", "END_COLON", "END_SEMICOLON",
    "END_PERIOD", "END_QUESTION", "END_EXCLAMATION", "END_PARAGRAPH",
    "END_CONTINUATION",
)
SEGMENTS = ((1, "AA"), (2, "AE"), (3, "B"), (4, "K"))

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


def _jsonl(*documents: JsonObject) -> bytes:
    return b"".join(_canonical(document) for document in documents)


def build_payloads(graph_bytes: bytes) -> tuple[dict[str, bytes], str]:
    inventory = b"".join(
        f"{segment_id}\t{name}\n".encode("ascii")
        for segment_id, name in SEGMENTS
    )
    inventory_sha = _sha256(inventory)

    fixture_words = sorted({
        "hello", *[chr(symbol) for symbol in range(ord("a"), ord("z") + 1)],
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "dash", "slash", "dot", "colon", "at",
        "underscore", "question", "equals", "and", "dollar", "greater",
        "than", "less", "tilde", "backslash", "hash", "percent", "plus",
        "star", "pipe", "exclamation", "left", "right", "bracket", "brace",
        "parenthesis", "comma", "semicolon", "caret", "backtick", "double",
        "single", "quote",
    })
    lexicon_entries: list[JsonObject] = [{
        "case": "ascii-fold",
        "grapheme": word,
        "roles": ["default"],
        "schema": "kilix.voicegen.pronunciation-entry/v1",
        "source": "first-party-test-fixture",
        "syllables": [{"segments": ["K"], "stress": "primary"}],
    } for word in fixture_words]
    lexicon = _jsonl({
        "admission": "test-fixture",
        "dialect": "en-AU",
        "entry_count": len(lexicon_entries),
        "resource_id": "kilix-en-au-runtime-lexicon-fixture-1",
        "review_record_sha256": None,
        "schema": "kilix.voicegen.pronunciation-lexicon/v1",
        "segment_inventory_sha256": inventory_sha,
    }, *lexicon_entries)
    lexicon_sha = _sha256(lexicon)

    roots = {chr(symbol): 0 for symbol in range(ord("a"), ord("z") + 1)}
    lts = _jsonl(
        {
            "admission": "test-fixture",
            "context_left": 0,
            "context_right": 1,
            "dialect": "en-AU",
            "maximum_steps": 2,
            "node_count": 3,
            "resource_id": "kilix-en-au-runtime-lts-fixture-1",
            "review_record_sha256": None,
            "roots": roots,
            "schema": "kilix.voicegen.lts-model/v1",
            "segment_inventory_sha256": inventory_sha,
            "source_lexicon_sha256": lexicon_sha,
            "training_record_sha256": _sha256(
                b"kilix-voicegen first-party runtime LTS fixture v1\n"
            ),
        },
        {
            "feature_offset": 1,
            "feature_value": "$",
            "id": 0,
            "kind": "decision",
            "no": 2,
            "schema": "kilix.voicegen.lts-node/v1",
            "yes": 1,
        },
        {
            "emissions": [{"segment_ids": [4], "syllable_end": "primary"}],
            "id": 1,
            "kind": "leaf",
            "schema": "kilix.voicegen.lts-node/v1",
        },
        {
            "emissions": [{"segment_ids": [4], "syllable_end": None}],
            "id": 2,
            "kind": "leaf",
            "schema": "kilix.voicegen.lts-node/v1",
        },
    )
    lts_sha = _sha256(lts)

    frontend_abi = _sha256(_canonical({
        "dialect": "en-AU",
        "lts_sha256": lts_sha,
        "pronunciation_lexicon_sha256": lexicon_sha,
        "schema": "kilix.voicegen.frontend-abi/v1",
        "segment_inventory_sha256": inventory_sha,
        "token_schema": "kilix.voicegen.tokens/v1",
        "unicode_version": "17.0.0",
    }))
    token_documents: list[JsonObject] = [{
        "admission": "test-fixture",
        "dialect": "en-AU",
        "entry_count": len(CONTROLS) + len(SEGMENTS),
        "frontend_abi_sha256": frontend_abi,
        "maximum_input_tokens": 512,
        "resource_id": "kilix-en-au-runtime-tokens-fixture-1",
        "schema": "kilix.voicegen.model-token-inventory/v1",
        "segment_inventory_sha256": inventory_sha,
    }]
    token_documents.extend({
        "id": token_id,
        "kind": "control",
        "name": name,
        "schema": "kilix.voicegen.model-token-entry/v1",
        "segment_id": None,
    } for token_id, name in enumerate(CONTROLS))
    token_documents.extend({
        "id": len(CONTROLS) + index,
        "kind": "segment",
        "name": name,
        "schema": "kilix.voicegen.model-token-entry/v1",
        "segment_id": segment_id,
    } for index, (segment_id, name) in enumerate(SEGMENTS))
    tokens = _jsonl(*token_documents)

    return ({
        "fixture.graph": graph_bytes,
        "frontend/segments.tsv": inventory,
        "frontend/pronunciation.jsonl": lexicon,
        "frontend/lts.jsonl": lts,
        "frontend/tokens.jsonl": tokens,
    }, frontend_abi)


def build_manifest(payloads: dict[str, bytes], frontend_abi: str) -> JsonObject:
    inventory = payloads["frontend/segments.tsv"]
    roles = {
        "fixture.graph": "fixture_graph",
        "frontend/segments.tsv": "segment_inventory",
        "frontend/pronunciation.jsonl": "pronunciation_lexicon",
        "frontend/lts.jsonl": "lts_model",
        "frontend/tokens.jsonl": "model_token_inventory",
    }
    files = [{
        "bytes": len(payloads[path]),
        "path": path,
        "role": roles[path],
        "sha256": _sha256(payloads[path]),
    } for path in sorted(payloads)]
    return {
        "audio": {"channels": 1, "sample_format": "s16", "sample_rate": SAMPLE_RATE},
        "determinism": {
            "class": "platform-independent-integer",
            "default_seed": str(DEFAULT_SEED),
            "test_vector_sha256": fixture_smoke_sha256(),
        },
        "engine": {"kind": "fixture-tone/v1"},
        "files": files,
        "frontend": {
            "abi_sha256": frontend_abi,
            "admission": "test-fixture",
            "dialect": "en-AU",
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
        "resources": {
            "minimum_memory_bytes": 1048576,
            "model_bytes": sum(len(payload) for payload in payloads.values()),
        },
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
    payloads, frontend_abi = build_payloads(graph_bytes)
    manifest = build_manifest(payloads, frontend_abi)
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

    for relative, payload in payloads.items():
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        _atomic_write(destination, payload)
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
