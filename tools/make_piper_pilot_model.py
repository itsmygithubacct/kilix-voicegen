#!/usr/bin/env python3
"""Build an external, research-only Kilix package around a Piper ONNX pilot."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import sys
import types
from typing import Any

ABI_VERSION = 1
OUTPUT_SAMPLE_RATE = 24_000
DEFAULT_SEED = 0x4B696C6978564731
CONTROLS = (
    "PAD", "BOS", "EOS", "WB", "SYL", "STRESS_0", "STRESS_1",
    "STRESS_2", "END_NONE", "END_COMMA", "END_COLON", "END_SEMICOLON",
    "END_PERIOD", "END_QUESTION", "END_EXCLAMATION", "END_PARAGRAPH",
    "END_CONTINUATION",
)
CONTROL_PROJECTIONS: dict[str, list[int]] = {
    "PAD": [],
    "BOS": [1, 0],
    "EOS": [2],
    "WB": [3, 0],
    "SYL": [],
    "STRESS_0": [],
    "STRESS_1": [],
    "STRESS_2": [],
    "END_NONE": [],
    "END_COMMA": [8, 0],
    "END_COLON": [11, 0],
    "END_SEMICOLON": [12, 0],
    "END_PERIOD": [10, 0],
    "END_QUESTION": [13, 0],
    "END_EXCLAMATION": [4, 0],
    "END_PARAGRAPH": [10, 0],
    "END_CONTINUATION": [8, 0],
}
DEFAULT_TEXTS = (
    "Hello from Kilix voicegen.",
    "This Australian female technical pilot is producing speech.",
    "The Australian female pilot can now produce clear synthetic speech.",
    "Welcome to Kilix voicegen.",
)
WORD = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)?")

JsonObject = dict[str, Any]


def _canonical(document: JsonObject) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


def _jsonl(*documents: JsonObject) -> bytes:
    return b"".join(_canonical(document) for document in documents)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _segment_name(phoneme: str) -> str:
    encoded = "P_" + "_".join(f"{ord(symbol):04X}" for symbol in phoneme)
    if len(encoded) > 32:
        raise ValueError(f"phoneme has no bounded stable segment name: {phoneme!r}")
    return encoded


def _atomic_write(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _atomic_copy(source: pathlib.Path, destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    with source.open("rb") as input_stream, temporary.open("wb") as output_stream:
        shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
        output_stream.flush()
        os.fsync(output_stream.fileno())
    os.replace(temporary, destination)


def _load_phonemizer(piper_source: pathlib.Path):
    source = piper_source.resolve()
    package_path = source / "piper"
    module_path = package_path / "phonemize_espeak.py"
    if not module_path.is_file():
        raise ValueError("--piper-source must contain the built Piper Python package")
    package_name = "_kgv_piper_" + hashlib.sha256(
        str(source).encode("utf-8")).hexdigest()[:16]
    package = types.ModuleType(package_name)
    package.__package__ = package_name
    package.__path__ = [str(package_path)]
    sys.modules[package_name] = package
    module_name = package_name + ".phonemize_espeak"
    specification = importlib.util.spec_from_file_location(module_name, module_path)
    if specification is None or specification.loader is None:
        raise ValueError("could not load the pinned Piper phonemizer module")
    module = importlib.util.module_from_spec(specification)
    sys.modules[module_name] = module
    specification.loader.exec_module(module)
    return module.EspeakPhonemizer()


def _phonemes_for_word(phonemizer, voice: str, word: str,
                       id_map: dict[str, list[int]]) -> list[str]:
    sentences = phonemizer.phonemize(voice, word)
    if len(sentences) != 1 or not sentences[0]:
        raise ValueError(f"eSpeak did not return one pronunciation for {word!r}")
    result = list(sentences[0])
    if len(result) > 32:
        raise ValueError(f"pilot pronunciation exceeds 32 segments: {word!r}")
    for phoneme in result:
        ids = id_map.get(phoneme)
        if not isinstance(ids, list) or len(ids) != 1 or not isinstance(ids[0], int):
            raise ValueError(f"Piper map has no single ID for phoneme {phoneme!r}")
        if not 4 <= ids[0] <= 255:
            raise ValueError(f"word pronunciation uses a reserved Piper ID: {phoneme!r}")
    return result


def _build_frontend(config: JsonObject, piper_source: pathlib.Path,
                    texts: list[str]) -> tuple[dict[str, bytes], str]:
    if config.get("phoneme_type") != "espeak":
        raise ValueError("pilot generator supports only Piper eSpeak phoneme models")
    voice = config.get("espeak", {}).get("voice")
    raw_map = config.get("phoneme_id_map")
    if not isinstance(voice, str) or not voice or not isinstance(raw_map, dict):
        raise ValueError("Piper config lacks eSpeak voice or phoneme ID map")
    id_map: dict[str, list[int]] = {}
    for phoneme, raw_ids in raw_map.items():
        if (not isinstance(phoneme, str) or not isinstance(raw_ids, list)
                or any(not isinstance(value, int) for value in raw_ids)):
            raise ValueError("Piper phoneme ID map has an invalid entry")
        id_map[phoneme] = raw_ids
    for special, expected in {"_": [0], "^": [1], "$": [2], " ": [3]}.items():
        if id_map.get(special) != expected:
            raise ValueError("Piper phoneme ID map does not use the v1 special IDs")

    vocabulary = sorted({match.group(0).lower()
                         for text in texts for match in WORD.finditer(text)})
    if not vocabulary:
        raise ValueError("pilot vocabulary is empty")
    phonemizer = _load_phonemizer(piper_source)
    pronunciations = {
        word: _phonemes_for_word(phonemizer, voice, word, id_map)
        for word in vocabulary
    }
    letter_pronunciations = {
        letter: _phonemes_for_word(phonemizer, voice, letter.upper(), id_map)
        for letter in "abcdefghijklmnopqrstuvwxyz"
    }
    phonemes = sorted({phoneme for phones in pronunciations.values()
                       for phoneme in phones}
                      | {phoneme for phones in letter_pronunciations.values()
                         for phoneme in phones})
    segments = [(index + 1, _segment_name(phoneme), phoneme)
                for index, phoneme in enumerate(phonemes)]
    names = {phoneme: name for _segment_id, name, phoneme in segments}
    segment_ids = {phoneme: segment_id
                   for segment_id, _name, phoneme in segments}
    inventory = b"".join(
        f"{segment_id}\t{name}\n".encode("ascii")
        for segment_id, name, _phoneme in segments
    )
    inventory_sha = _sha256(inventory)

    lexicon_entries: list[JsonObject] = []
    for word in vocabulary:
        lexicon_entries.append({
            "case": "ascii-fold",
            "grapheme": word,
            "roles": ["default"],
            "schema": "kilix.voicegen.pronunciation-entry/v1",
            "source": "piper-espeak-pilot",
            "syllables": [{
                "segments": [names[phoneme] for phoneme in pronunciations[word]],
                "stress": "none",
            }],
        })
    lexicon = _jsonl({
        "admission": "test-fixture",
        "dialect": "en-AU",
        "entry_count": len(lexicon_entries),
        "resource_id": "kilix-en-au-piper-pilot-lexicon-1",
        "review_record_sha256": None,
        "schema": "kilix.voicegen.pronunciation-lexicon/v1",
        "segment_inventory_sha256": inventory_sha,
    }, *lexicon_entries)
    lexicon_sha = _sha256(lexicon)

    roots = {letter: index for index, letter in enumerate("abcdefghijklmnopqrstuvwxyz")}
    lts_nodes = [{
        "emissions": [{
            "segment_ids": [segment_ids[phoneme]
                            for phoneme in letter_pronunciations[letter]],
            "syllable_end": "none",
        }],
        "id": index,
        "kind": "leaf",
        "schema": "kilix.voicegen.lts-node/v1",
    } for index, letter in enumerate("abcdefghijklmnopqrstuvwxyz")]
    lts = _jsonl({
        "admission": "test-fixture",
        "context_left": 0,
        "context_right": 0,
        "dialect": "en-AU",
        "maximum_steps": 1,
        "node_count": len(lts_nodes),
        "resource_id": "kilix-en-au-piper-pilot-lts-1",
        "review_record_sha256": None,
        "roots": roots,
        "schema": "kilix.voicegen.lts-model/v1",
        "segment_inventory_sha256": inventory_sha,
        "source_lexicon_sha256": lexicon_sha,
        "training_record_sha256": _sha256(
            b"research-only eSpeak letter fallback; not product G2P\n"),
    }, *lts_nodes)
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
    token_entries: list[JsonObject] = []
    for token_id, name in enumerate(CONTROLS):
        token_entries.append({
            "id": token_id,
            "kind": "control",
            "name": name,
            "schema": "kilix.voicegen.model-token-entry/v1",
            "segment_id": None,
        })
    for index, (segment_id, name, _phoneme) in enumerate(segments):
        token_entries.append({
            "id": len(CONTROLS) + index,
            "kind": "segment",
            "name": name,
            "schema": "kilix.voicegen.model-token-entry/v1",
            "segment_id": segment_id,
        })
    tokens = _jsonl({
        "admission": "test-fixture",
        "dialect": "en-AU",
        "entry_count": len(token_entries),
        "frontend_abi_sha256": frontend_abi,
        "maximum_input_tokens": 512,
        "resource_id": "kilix-en-au-piper-pilot-tokens-1",
        "schema": "kilix.voicegen.model-token-inventory/v1",
        "segment_inventory_sha256": inventory_sha,
    }, *token_entries)
    tokens_sha = _sha256(tokens)

    projections: list[JsonObject] = []
    for source_id, name in enumerate(CONTROLS):
        projections.append({
            "schema": "kilix.voicegen.token-projection-entry/v1",
            "source_id": source_id,
            "target_ids": CONTROL_PROJECTIONS[name],
        })
    for index, (_segment_id, _name, phoneme) in enumerate(segments):
        projections.append({
            "schema": "kilix.voicegen.token-projection-entry/v1",
            "source_id": len(CONTROLS) + index,
            "target_ids": [id_map[phoneme][0], 0],
        })
    projection = _jsonl({
        "engine_kind": "piper-vits-onnx/v1",
        "entry_count": len(projections),
        "maximum_output_tokens": 2048,
        "resource_id": "kilix-piper-pilot-projection-1",
        "schema": "kilix.voicegen.token-projection/v1",
        "source_token_inventory_sha256": tokens_sha,
        "target_id_max": 255,
    }, *projections)

    return ({
        "frontend/segments.tsv": inventory,
        "frontend/pronunciation.jsonl": lexicon,
        "frontend/lts.jsonl": lts,
        "frontend/tokens.jsonl": tokens,
        "frontend/piper_projection.jsonl": projection,
    }, frontend_abi)


def create_package(output: pathlib.Path, onnx_path: pathlib.Path,
                   config_path: pathlib.Path, piper_source: pathlib.Path,
                   texts: list[str]) -> str:
    if output.is_symlink():
        raise ValueError("pilot output directory must not be a symbolic link")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("pilot output directory must be absent or empty")
    if onnx_path.is_symlink() or not onnx_path.is_file():
        raise ValueError("--onnx must be a regular non-symlink file")
    if config_path.is_symlink() or not config_path.is_file():
        raise ValueError("--config must be a regular non-symlink file")
    config_bytes = config_path.read_bytes()
    config = json.loads(config_bytes)
    if (config.get("audio") != {"sample_rate": 22050}
            or config.get("num_symbols") != 256
            or config.get("num_speakers") != 1):
        raise ValueError("pilot config is not the locked 22.05 kHz single-speaker shape")
    inference = config.get("inference", {})
    source_noise_scale_milli = round(float(inference.get("noise_scale", 0.0)) * 1000)
    source_noise_w_milli = round(float(inference.get("noise_w", 0.0)) * 1000)
    if (source_noise_scale_milli, source_noise_w_milli) != (667, 800):
        raise ValueError("pilot config inference scales do not match the locked model")
    # The ONNX C API has no random-seed setter. Zeroing both stochastic scales
    # makes repeated ABI-v1 jobs deterministic without private ORT symbols.
    noise_scale_milli = 0
    noise_w_milli = 0

    payloads, frontend_abi = _build_frontend(config, piper_source, texts)
    notice = (
        "KILIX VOICEGEN RESEARCH PILOT — NOT FOR RELEASE\n\n"
        "This package is an internal technical interoperability control, not the "
        "release voice kilix-female-01. The adapted recording source is Barbara "
        "Baker's solo LibriVox reading of The Novels of Jane Austen. LibriVox "
        "states that volunteer recordings are donated to the public domain; the "
        "Internet Archive item carries Public Domain Mark 1.0. The source is not "
        "an executed synthetic-voice consent/release and does not clear identity, "
        "personality, or worldwide neighboring-right questions for distribution.\n\n"
        "The graph was warm-started from Piper en_GB-cori-medium. Its complete "
        "voice/data derivative lineage has not been cleared for a Kilix product "
        "release. The package therefore remains research-only even where the "
        "underlying recording is public domain in the United States.\n\n"
        "Source: https://librivox.org/the-novels-of-jane-austen-by-george-henry-lewes/\n"
        "Public-domain guidance: https://wiki.librivox.org/index.php/Copyright_and_Public_Domain\n"
    ).encode("utf-8")
    payloads["NOTICE.research.txt"] = notice

    roles = {
        "model.onnx": "onnx_model",
        "frontend/segments.tsv": "segment_inventory",
        "frontend/pronunciation.jsonl": "pronunciation_lexicon",
        "frontend/lts.jsonl": "lts_model",
        "frontend/tokens.jsonl": "model_token_inventory",
        "frontend/piper_projection.jsonl": "token_projection",
        "NOTICE.research.txt": "license_notice",
    }
    onnx_bytes = onnx_path.stat().st_size
    onnx_sha = _file_sha256(onnx_path)
    files = [{
        "bytes": onnx_bytes,
        "path": "model.onnx",
        "role": roles["model.onnx"],
        "sha256": onnx_sha,
    }]
    files.extend({
        "bytes": len(payloads[path]),
        "path": path,
        "role": roles[path],
        "sha256": _sha256(payloads[path]),
    } for path in sorted(payloads))
    inventory = payloads["frontend/segments.tsv"]
    segment_ids = [int(line.split(b"\t", 1)[0])
                   for line in inventory.rstrip(b"\n").split(b"\n")]
    manifest: JsonObject = {
        "audio": {"channels": 1, "sample_format": "s16",
                  "sample_rate": OUTPUT_SAMPLE_RATE},
        "determinism": {
            "class": "platform-class",
            "default_seed": str(DEFAULT_SEED),
            "test_vector_sha256": _sha256(_canonical({
                "model_sha256": onnx_sha,
                "projection_sha256": _sha256(
                    payloads["frontend/piper_projection.jsonl"]),
                "text": DEFAULT_TEXTS[0],
            })),
        },
        "engine": {
            "kind": "piper-vits-onnx/v1",
            "model_sample_rate": 22050,
            "noise_scale_milli": noise_scale_milli,
            "noise_w_milli": noise_w_milli,
            "target_id_max": 255,
        },
        "files": sorted(files, key=lambda item: item["path"]),
        "frontend": {
            "abi_sha256": frontend_abi,
            "admission": "test-fixture",
            "dialect": "en-AU",
            "inventory_sha256": _sha256(inventory),
            "schema": "kilix.voicegen.frontend/v1",
            "segment_ids": segment_ids,
            "token_schema": "kilix.voicegen.tokens/v1",
            "unicode_version": "17.0.0",
        },
        "licenses": [{
            "component": "Barbara Baker LibriVox reference adaptation",
            "license": "Public Domain Mark 1.0 source designation; package internal-research restriction",
            "notice_path": "NOTICE.research.txt",
        }, {
            "component": "Piper en_GB-cori-medium warm-start lineage",
            "license": "Not cleared for Kilix product redistribution",
            "notice_path": "NOTICE.research.txt",
        }],
        "limitations": [
            "Research-only technical pilot; not a release-approved Kilix voice.",
            "Frontend lexicon covers only the declared pilot phrases; fallback is letter speech.",
            "Uses a 22.05 kHz model resampled to the fixed 24 kHz ABI output.",
            "Runs the stochastic Piper scales at zero for repeatable C-ABI output.",
            "Speaker consent, identity/personality review, and warm-start lineage remain open.",
        ],
        "model_id": "kilix-au-female-reference-control-ref-001",
        "quantization": {
            "policy": "Unquantized research-control Piper VITS graph.",
            "precision": "fp32",
        },
        "required_cpu_features": ["sse2"],
        "resources": {
            "minimum_memory_bytes": 536870912,
            "model_bytes": onnx_bytes + sum(len(value) for value in payloads.values()),
        },
        "revisions": {
            "architecture": "piper-vits-onnx/v1",
            "export": f"onnx-sha256:{onnx_sha}",
            "training": "barbara-baker-ref-001-internal-control",
        },
        "runtime_abi": {"maximum": ABI_VERSION, "minimum": ABI_VERSION},
        "schema": "kilix.voicegen.model/v1",
        "tensors": [
            {"dtype": "int64", "io": "input", "name": "input",
             "shape": [1, "N"]},
            {"dtype": "int64", "io": "input", "name": "input_lengths",
             "shape": [1]},
            {"dtype": "float32", "io": "input", "name": "scales",
             "shape": [3]},
            {"dtype": "float32", "io": "output", "name": "output",
             "shape": [1, 1, 1, "T"]},
        ],
        "version": "0.0.0-research-ref-001",
        "voices": [{
            "id": "kilix-female-01",
            "label": "Australian Female Research Control (not release voice)",
        }],
    }
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

    output.mkdir(parents=True, exist_ok=True)
    _atomic_copy(onnx_path, output / "model.onnx")
    for relative, payload in payloads.items():
        _atomic_write(output / relative, payload)
    _atomic_write(output / "MANIFEST.json", manifest_bytes)
    _atomic_write(output / "RELEASE.json", release_bytes)
    _atomic_write(output / "RELEASE.sha256", (release_sha + "\n").encode("ascii"))
    return release_sha


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--onnx", type=pathlib.Path, required=True)
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--piper-source", type=pathlib.Path, required=True,
                        help="directory containing built piper Python package")
    parser.add_argument("--text", action="append", default=[],
                        help="admit every simple ASCII word in this pilot text")
    arguments = parser.parse_args()
    texts = list(DEFAULT_TEXTS)
    texts.extend(arguments.text)
    release_sha = create_package(arguments.output, arguments.onnx, arguments.config,
                                 arguments.piper_source, texts)
    print(release_sha)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
