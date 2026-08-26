#!/usr/bin/env python3
"""Train a deterministic compact LTS decision forest from aligned JSONL."""

from __future__ import annotations

import argparse
import collections
import dataclasses
from fractions import Fraction
import hashlib
import json
import os
import pathlib
import re
import shutil
import string
import tempfile
from typing import Any, Iterable


ALIGNED_HEADER_SCHEMA = "kilix.voicegen.lts-aligned-corpus/v1"
ALIGNED_ENTRY_SCHEMA = "kilix.voicegen.lts-aligned-entry/v1"
MODEL_SCHEMA = "kilix.voicegen.lts-model/v1"
NODE_SCHEMA = "kilix.voicegen.lts-node/v1"
TRAINING_SCHEMA = "kilix.voicegen.lts-training-record/v1"
MANIFEST_SCHEMA = "kilix.voicegen.lts-training-manifest/v1"
ADMISSIONS = {"product-admitted", "local-user", "test-fixture"}
STRESSES = {"none", "primary", "secondary"}
WORD_SYMBOLS = set(string.ascii_lowercase + "'-")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")
IDENTIFIER = re.compile(r"[a-z][a-z0-9._:/-]{0,127}\Z")
SEGMENT_NAME = re.compile(r"[A-Z][A-Z0-9_]{0,31}\Z")
MAX_RESOURCE_BYTES = 256 * 1024 * 1024
MAX_LINE_BYTES = 64 * 1024
MAX_ENTRIES = 200_000
MAX_WORD_BYTES = 256
MAX_CONTEXT = 8
MAX_DEPTH = 63
MAX_NODES = 500_000

JsonObject = dict[str, Any]
EmissionPart = tuple[tuple[int, ...], str | None]
EmissionLabel = tuple[EmissionPart, ...]
Tree = tuple[Any, ...]


def strict_object(pairs: list[tuple[str, Any]]) -> JsonObject:
    result: JsonObject = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def canonical(document: JsonObject) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def exact_keys(document: JsonObject, expected: set[str], description: str) -> None:
    if set(document) != expected:
        raise ValueError(f"{description} has unknown or absent fields")


def require_int(value: Any, minimum: int, maximum: int, description: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{description} must be an integer")
    if not minimum <= value <= maximum:
        raise ValueError(f"{description} is outside [{minimum}, {maximum}]")
    return value


def require_sha(value: Any, description: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise ValueError(f"{description} must be lowercase SHA-256")
    return value


def read_verified(path: pathlib.Path, expected_sha256: str,
                  description: str = "aligned input",
                  maximum_bytes: int = MAX_RESOURCE_BYTES) -> bytes:
    require_sha(expected_sha256, f"expected {description} hash")
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{description} is absent, non-regular, or symbolic")
    size = path.stat().st_size
    if size == 0 or size > maximum_bytes:
        raise ValueError(f"{description} is empty or exceeds its byte limit")
    data = path.read_bytes()
    actual = digest(data)
    if actual != expected_sha256:
        raise ValueError(
            f"{description} SHA-256 mismatch: expected {expected_sha256}, got {actual}")
    return data


def parse_jsonl(data: bytes) -> list[JsonObject]:
    if not data.endswith(b"\n") or b"\r" in data:
        raise ValueError("aligned JSONL must use LF and end with LF")
    documents: list[JsonObject] = []
    for line_number, line in enumerate(data.splitlines(), 1):
        if not line or len(line) > MAX_LINE_BYTES:
            raise ValueError(f"line {line_number}: empty or oversized JSONL line")
        try:
            document = json.loads(line, object_pairs_hook=strict_object)
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise ValueError(f"line {line_number}: invalid strict JSON: {error}") from error
        if not isinstance(document, dict):
            raise ValueError(f"line {line_number}: JSON value must be an object")
        if canonical(document).removesuffix(b"\n") != line:
            raise ValueError(f"line {line_number}: JSON is not in canonical form")
        documents.append(document)
    return documents


def validate_word(word: Any) -> str:
    if not isinstance(word, str):
        raise ValueError("aligned word must be a string")
    encoded = word.encode("utf-8")
    if not encoded or len(encoded) > MAX_WORD_BYTES or any(ord(char) > 127 for char in word):
        raise ValueError("aligned word must be 1-256 ASCII bytes")
    if word[0] not in string.ascii_lowercase or word[-1] not in string.ascii_lowercase:
        raise ValueError("aligned word must begin and end with a lowercase letter")
    for index, symbol in enumerate(word):
        if symbol not in WORD_SYMBOLS:
            raise ValueError("aligned word contains a noncanonical symbol")
        if symbol in "'-" and (index == 0 or index + 1 == len(word) or
                               word[index - 1] not in string.ascii_lowercase or
                               word[index + 1] not in string.ascii_lowercase):
            raise ValueError("aligned punctuation must separate lowercase letters")
    return word


def parse_emission(value: Any, inventory_ids: set[int]) -> EmissionLabel:
    if not isinstance(value, list) or len(value) > 4:
        raise ValueError("per-symbol emissions must be an array of at most four parts")
    result: list[EmissionPart] = []
    total = 0
    for part in value:
        if not isinstance(part, dict):
            raise ValueError("emission part must be an object")
        exact_keys(part, {"segment_ids", "syllable_end"}, "emission part")
        raw_segments = part["segment_ids"]
        if not isinstance(raw_segments, list) or not 1 <= len(raw_segments) <= 16:
            raise ValueError("emission part must contain 1-16 segment IDs")
        segments: list[int] = []
        for raw_segment in raw_segments:
            segment = require_int(raw_segment, 1, 65535, "emission segment ID")
            if segment not in inventory_ids:
                raise ValueError("emission segment ID is outside the inventory")
            segments.append(segment)
        total += len(segments)
        if total > 32:
            raise ValueError("per-symbol emissions exceed 32 segments")
        stress = part["syllable_end"]
        if stress is not None and stress not in STRESSES:
            raise ValueError("emission syllable_end is invalid")
        result.append((tuple(segments), stress))
    return tuple(result)


def validate_pronunciation(labels: tuple[EmissionLabel, ...]) -> None:
    open_segments = 0
    total_segments = 0
    syllables = 0
    primary = 0
    for label in labels:
        for segments, stress in label:
            open_segments += len(segments)
            total_segments += len(segments)
            if total_segments > 128:
                raise ValueError("aligned pronunciation exceeds 128 segments")
            if stress is not None:
                if open_segments == 0:
                    raise ValueError("aligned pronunciation has an empty syllable")
                syllables += 1
                if syllables > 16:
                    raise ValueError("aligned pronunciation exceeds 16 syllables")
                if stress == "primary":
                    primary += 1
                    if primary > 1:
                        raise ValueError("aligned pronunciation has multiple primary stresses")
                open_segments = 0
    if open_segments or syllables == 0:
        raise ValueError("aligned pronunciation is empty or has an open syllable")


@dataclasses.dataclass(frozen=True)
class AlignedEntry:
    word: str
    emissions: tuple[EmissionLabel, ...]


@dataclasses.dataclass(frozen=True)
class AlignedCorpus:
    admission: str
    alignment_record_sha256: str
    license_record_sha256: str
    review_record_sha256: str | None
    segment_inventory: tuple[tuple[str, int], ...]
    segment_inventory_sha256: str
    source_lexicon_sha256: str
    entries: tuple[AlignedEntry, ...]


def parse_corpus(data: bytes, required_admission: str) -> AlignedCorpus:
    documents = parse_jsonl(data)
    if len(documents) < 2:
        raise ValueError("aligned corpus requires a header and at least one entry")
    header = documents[0]
    exact_keys(header, {
        "admission", "alignment_record_sha256", "dialect", "entry_count",
        "license_record_sha256", "review_record_sha256", "schema",
        "segment_inventory", "segment_inventory_sha256",
        "source_lexicon_sha256",
    }, "aligned corpus header")
    if header["schema"] != ALIGNED_HEADER_SCHEMA or header["dialect"] != "en-AU":
        raise ValueError("aligned corpus schema or dialect is unsupported")
    admission = header["admission"]
    if admission not in ADMISSIONS or admission != required_admission:
        raise ValueError("aligned corpus admission does not match the caller")
    source_lexicon_sha256 = require_sha(header["source_lexicon_sha256"],
                                        "source lexicon hash")
    alignment_record_sha256 = require_sha(header["alignment_record_sha256"],
                                          "alignment record hash")
    license_record_sha256 = require_sha(header["license_record_sha256"],
                                        "license record hash")
    raw_review = header["review_record_sha256"]
    if admission == "product-admitted":
        review_record_sha256: str | None = require_sha(raw_review, "review record hash")
    else:
        if raw_review is not None:
            raise ValueError("non-product aligned corpus must not claim review")
        review_record_sha256 = None

    raw_inventory = header["segment_inventory"]
    if not isinstance(raw_inventory, list) or not raw_inventory:
        raise ValueError("aligned segment inventory must be a nonempty array")
    inventory: list[tuple[str, int]] = []
    inventory_ids: set[int] = set()
    inventory_names: set[str] = set()
    previous_id = 0
    inventory_bytes = bytearray()
    for raw_segment in raw_inventory:
        if not isinstance(raw_segment, dict):
            raise ValueError("aligned segment definition must be an object")
        exact_keys(raw_segment, {"id", "name"}, "aligned segment definition")
        segment_id = require_int(raw_segment["id"], 1, 65535, "segment ID")
        name = raw_segment["name"]
        if not isinstance(name, str) or SEGMENT_NAME.fullmatch(name) is None:
            raise ValueError("segment name is not canonical")
        if segment_id <= previous_id or segment_id in inventory_ids or name in inventory_names:
            raise ValueError("segment inventory must have increasing IDs and unique names")
        inventory_ids.add(segment_id)
        inventory_names.add(name)
        inventory.append((name, segment_id))
        inventory_bytes.extend(f"{segment_id}\t{name}\n".encode("ascii"))
        previous_id = segment_id
    inventory_sha256 = require_sha(header["segment_inventory_sha256"],
                                    "segment inventory hash")
    if digest(bytes(inventory_bytes)) != inventory_sha256:
        raise ValueError("segment inventory hash does not match its definitions")

    count = require_int(header["entry_count"], 1, MAX_ENTRIES, "entry count")
    if count != len(documents) - 1:
        raise ValueError("aligned corpus entry count does not match the header")
    entries: list[AlignedEntry] = []
    previous_word = ""
    for document in documents[1:]:
        exact_keys(document, {"emissions", "schema", "word"}, "aligned entry")
        if document["schema"] != ALIGNED_ENTRY_SCHEMA:
            raise ValueError("aligned entry schema is unsupported")
        word = validate_word(document["word"])
        if previous_word and word <= previous_word:
            raise ValueError("aligned entries must have unique sorted words")
        previous_word = word
        raw_emissions = document["emissions"]
        if not isinstance(raw_emissions, list) or len(raw_emissions) != len(word):
            raise ValueError("aligned entry requires one emission per word symbol")
        emissions = tuple(parse_emission(value, inventory_ids)
                          for value in raw_emissions)
        validate_pronunciation(emissions)
        entries.append(AlignedEntry(word, emissions))
    return AlignedCorpus(
        admission=admission,
        alignment_record_sha256=alignment_record_sha256,
        license_record_sha256=license_record_sha256,
        review_record_sha256=review_record_sha256,
        segment_inventory=tuple(inventory),
        segment_inventory_sha256=inventory_sha256,
        source_lexicon_sha256=source_lexicon_sha256,
        entries=tuple(entries),
    )


@dataclasses.dataclass(frozen=True)
class Example:
    word: str
    position: int
    label: EmissionLabel


def context_symbol(example: Example, offset: int) -> str:
    target = example.position + offset
    if target < 0:
        return "^"
    if target >= len(example.word):
        return "$"
    return example.word[target]


def majority_label(examples: tuple[Example, ...]) -> EmissionLabel:
    counts = collections.Counter(example.label for example in examples)
    return min(counts, key=lambda label: (
        -counts[label],
        json.dumps(label_json(label), ensure_ascii=True, separators=(",", ":"),
                   sort_keys=True),
    ))


def purity(examples: tuple[Example, ...]) -> Fraction:
    counts = collections.Counter(example.label for example in examples)
    return Fraction(sum(count * count for count in counts.values()), len(examples))


def train_tree(examples: tuple[Example, ...], offsets: tuple[int, ...],
               depth: int, max_depth: int, min_leaf: int) -> Tree:
    labels = {example.label for example in examples}
    fallback = majority_label(examples)
    if len(labels) == 1 or depth >= max_depth:
        return ("leaf", fallback)
    base = purity(examples)
    best_score = base
    best: tuple[int, str, tuple[Example, ...], tuple[Example, ...]] | None = None
    for offset in offsets:
        values = sorted({context_symbol(example, offset) for example in examples})
        for value in values:
            yes = tuple(example for example in examples
                        if context_symbol(example, offset) == value)
            no = tuple(example for example in examples
                       if context_symbol(example, offset) != value)
            if len(yes) < min_leaf or len(no) < min_leaf:
                continue
            score = purity(yes) + purity(no)
            if score > best_score:
                best_score = score
                best = (offset, value, yes, no)
    if best is None:
        return ("leaf", fallback)
    offset, value, yes, no = best
    return (
        "decision", offset, value,
        train_tree(yes, offsets, depth + 1, max_depth, min_leaf),
        train_tree(no, offsets, depth + 1, max_depth, min_leaf),
    )


def label_json(label: EmissionLabel) -> list[JsonObject]:
    return [
        {"segment_ids": list(segment_ids), "syllable_end": stress}
        for segment_ids, stress in label
    ]


def compile_forest(trees: dict[str, Tree]) -> tuple[dict[str, int], list[JsonObject], int]:
    nodes: list[JsonObject] = []
    interned: dict[tuple[Any, ...], int] = {}

    def intern(tree: Tree) -> int:
        if tree[0] == "leaf":
            key = tree
            document: JsonObject = {
                "emissions": label_json(tree[1]),
                "id": -1,
                "kind": "leaf",
                "schema": NODE_SCHEMA,
            }
        else:
            yes = intern(tree[3])
            no = intern(tree[4])
            key = ("decision", tree[1], tree[2], yes, no)
            document = {
                "feature_offset": tree[1],
                "feature_value": tree[2],
                "id": -1,
                "kind": "decision",
                "no": no,
                "schema": NODE_SCHEMA,
                "yes": yes,
            }
        found = interned.get(key)
        if found is not None:
            return found
        node_id = len(nodes)
        if node_id >= MAX_NODES:
            raise ValueError("trained LTS forest exceeds 500,000 nodes")
        document["id"] = node_id
        nodes.append(document)
        interned[key] = node_id
        return node_id

    roots = {symbol: intern(trees[symbol]) for symbol in sorted(trees)}

    depths: dict[int, int] = {}

    def node_depth(node_id: int) -> int:
        found = depths.get(node_id)
        if found is not None:
            return found
        node = nodes[node_id]
        result = 1 if node["kind"] == "leaf" else 1 + max(
            node_depth(node["yes"]), node_depth(node["no"]))
        depths[node_id] = result
        return result

    maximum_steps = max(node_depth(root) for root in roots.values())
    return roots, nodes, maximum_steps


def split_entries(entries: tuple[AlignedEntry, ...], seed: str,
                  heldout_percent: int) -> tuple[tuple[AlignedEntry, ...],
                                                  tuple[AlignedEntry, ...]]:
    train: list[AlignedEntry] = []
    heldout: list[AlignedEntry] = []
    for entry in entries:
        key = hashlib.sha256(seed.encode("ascii") + b"\0" +
                             entry.word.encode("ascii")).digest()
        bucket = int.from_bytes(key[:8], "big") % 100
        (heldout if bucket < heldout_percent else train).append(entry)
    if not train or not heldout:
        raise ValueError("deterministic split produced an empty train or held-out set")
    return tuple(train), tuple(heldout)


def train_forest(entries: tuple[AlignedEntry, ...], left: int, right: int,
                 max_depth: int, min_leaf: int) -> tuple[dict[str, int],
                                                        list[JsonObject], int]:
    examples_by_symbol: dict[str, list[Example]] = collections.defaultdict(list)
    for entry in entries:
        for position, label in enumerate(entry.emissions):
            examples_by_symbol[entry.word[position]].append(
                Example(entry.word, position, label))
    offsets = tuple(range(-left, 0)) + tuple(range(1, right + 1))
    trees = {
        symbol: train_tree(tuple(examples), offsets, 0, max_depth, min_leaf)
        for symbol, examples in sorted(examples_by_symbol.items())
    }
    return compile_forest(trees)


def predict_label(word: str, position: int, roots: dict[str, int],
                  nodes: list[JsonObject]) -> EmissionLabel | None:
    node_id = roots.get(word[position])
    if node_id is None:
        return None
    while True:
        node = nodes[node_id]
        if node["kind"] == "leaf":
            return tuple((tuple(part["segment_ids"]), part["syllable_end"])
                         for part in node["emissions"])
        target = position + node["feature_offset"]
        value = "^" if target < 0 else "$" if target >= len(word) else word[target]
        node_id = node["yes"] if value == node["feature_value"] else node["no"]


def flattened(labels: Iterable[EmissionLabel]) -> list[int]:
    return [segment for label in labels for part, _stress in label for segment in part]


def edit_distance(reference: list[int], candidate: list[int]) -> int:
    previous = list(range(len(candidate) + 1))
    for ref_index, ref_value in enumerate(reference, 1):
        current = [ref_index]
        for cand_index, cand_value in enumerate(candidate, 1):
            current.append(min(
                current[-1] + 1,
                previous[cand_index] + 1,
                previous[cand_index - 1] + (ref_value != cand_value),
            ))
        previous = current
    return previous[-1]


def metrics(entries: tuple[AlignedEntry, ...], roots: dict[str, int],
            nodes: list[JsonObject]) -> JsonObject:
    exact_words = 0
    exact_symbols = 0
    invalid_emission_words = 0
    unresolved_words = 0
    symbol_count = 0
    segment_reference = 0
    segment_edits = 0
    for entry in entries:
        predicted: list[EmissionLabel] = []
        unresolved = False
        for position, reference_label in enumerate(entry.emissions):
            candidate_label = predict_label(entry.word, position, roots, nodes)
            symbol_count += 1
            if candidate_label is None:
                unresolved = True
                predicted.append(tuple())
            else:
                predicted.append(candidate_label)
                if candidate_label == reference_label:
                    exact_symbols += 1
        if unresolved:
            unresolved_words += 1
        predicted_tuple = tuple(predicted)
        if not unresolved:
            try:
                validate_pronunciation(predicted_tuple)
            except ValueError:
                invalid_emission_words += 1
        if not unresolved and predicted_tuple == entry.emissions:
            exact_words += 1
        reference_segments = flattened(entry.emissions)
        predicted_segments = flattened(predicted_tuple)
        segment_reference += len(reference_segments)
        segment_edits += edit_distance(reference_segments, predicted_segments)
    return {
        "exact_symbols": exact_symbols,
        "exact_words": exact_words,
        "invalid_emission_words": invalid_emission_words,
        "segment_edits": segment_edits,
        "segment_error_ppm": (
            (segment_edits * 1_000_000 + segment_reference // 2) // segment_reference
            if segment_reference else 0
        ),
        "segment_reference": segment_reference,
        "symbols": symbol_count,
        "unresolved_words": unresolved_words,
        "words": len(entries),
    }


def train(corpus: AlignedCorpus, input_data: bytes, input_sha256: str,
          generator_revision: str, resource_id: str, split_seed: str,
          heldout_percent: int, context_left: int, context_right: int,
          max_depth: int, min_leaf: int) -> tuple[bytes, bytes, bytes]:
    train_entries, heldout_entries = split_entries(corpus.entries, split_seed,
                                                    heldout_percent)
    roots, nodes, maximum_steps = train_forest(
        train_entries, context_left, context_right, max_depth, min_leaf)
    if corpus.admission == "product-admitted":
        missing = sorted(set(string.ascii_lowercase) - set(roots))
        if missing:
            raise ValueError("product LTS training split lacks roots: " + "".join(missing))
    train_metrics = metrics(train_entries, roots, nodes)
    heldout_metrics = metrics(heldout_entries, roots, nodes)
    if corpus.admission == "product-admitted" and heldout_metrics["unresolved_words"]:
        raise ValueError("product LTS held-out set contains unresolved symbols")
    if corpus.admission == "product-admitted" and (
            train_metrics["invalid_emission_words"] or
            heldout_metrics["invalid_emission_words"]):
        raise ValueError("product LTS emits an invalid syllable sequence")

    generator_data = pathlib.Path(__file__).resolve().read_bytes()
    training_record: JsonObject = {
        "admission": corpus.admission,
        "aligned_corpus": {
            "bytes": len(input_data),
            "entries": len(corpus.entries),
            "sha256": input_sha256,
        },
        "alignment_record_sha256": corpus.alignment_record_sha256,
        "config": {
            "context_left": context_left,
            "context_right": context_right,
            "criterion": "weighted-gini-purity/v1",
            "heldout_percent": heldout_percent,
            "leaf_policy": "majority-count-then-canonical-emission/v1",
            "max_depth": max_depth,
            "min_leaf": min_leaf,
            "split_algorithm": "sha256-seed-nul-word-mod-100/v1",
            "split_seed": split_seed,
            "tie_break": "offset-then-symbol-first-strict-improvement/v1",
        },
        "dialect": "en-AU",
        "generator": {
            "revision": generator_revision,
            "sha256": digest(generator_data),
        },
        "license_record_sha256": corpus.license_record_sha256,
        "metrics": {
            "heldout": heldout_metrics,
            "train": train_metrics,
        },
        "model_shape": {
            "maximum_steps": maximum_steps,
            "nodes": len(nodes),
            "roots": len(roots),
        },
        "resource_id": resource_id,
        "review_record_sha256": corpus.review_record_sha256,
        "schema": TRAINING_SCHEMA,
        "segment_inventory_sha256": corpus.segment_inventory_sha256,
        "source_lexicon_sha256": corpus.source_lexicon_sha256,
        "status": ("product-admitted" if corpus.admission == "product-admitted"
                   else "non-product-training-fixture"),
    }
    training_data = canonical(training_record)
    header: JsonObject = {
        "admission": corpus.admission,
        "context_left": context_left,
        "context_right": context_right,
        "dialect": "en-AU",
        "maximum_steps": maximum_steps,
        "node_count": len(nodes),
        "resource_id": resource_id,
        "review_record_sha256": corpus.review_record_sha256,
        "roots": roots,
        "schema": MODEL_SCHEMA,
        "segment_inventory_sha256": corpus.segment_inventory_sha256,
        "source_lexicon_sha256": corpus.source_lexicon_sha256,
        "training_record_sha256": digest(training_data),
    }
    model_data = canonical(header) + b"".join(canonical(node) for node in nodes)
    files = {
        "TRAINING.json": training_data,
        "model.jsonl": model_data,
    }
    manifest: JsonObject = {
        "admission": corpus.admission,
        "files": {
            name: {"bytes": len(data), "sha256": digest(data)}
            for name, data in sorted(files.items())
        },
        "resource_id": resource_id,
        "schema": MANIFEST_SCHEMA,
        "status": ("product-admitted" if corpus.admission == "product-admitted"
                   else "non-product-training-fixture"),
    }
    return model_data, training_data, canonical(manifest)


def write_output(output: pathlib.Path, model_data: bytes, training_data: bytes,
                 manifest_data: bytes) -> None:
    if output.exists() or output.is_symlink():
        raise ValueError("output directory already exists; refusing to overwrite it")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=f".{output.name}.",
                                               dir=output.parent))
    try:
        (temporary / "model.jsonl").write_bytes(model_data)
        (temporary / "TRAINING.json").write_bytes(training_data)
        (temporary / "MANIFEST.json").write_bytes(manifest_data)
        os.replace(temporary, output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--required-admission", choices=sorted(ADMISSIONS),
                        required=True)
    parser.add_argument("--generator-revision", required=True)
    parser.add_argument("--resource-id", required=True)
    parser.add_argument("--source-lexicon", type=pathlib.Path)
    parser.add_argument("--alignment-record", type=pathlib.Path)
    parser.add_argument("--license-record", type=pathlib.Path)
    parser.add_argument("--review-record", type=pathlib.Path)
    parser.add_argument("--split-seed", default="kilix-en-au-lts-split-1")
    parser.add_argument("--heldout-percent", type=int, default=10)
    parser.add_argument("--context-left", type=int, default=4)
    parser.add_argument("--context-right", type=int, default=4)
    parser.add_argument("--max-depth", type=int, default=16)
    parser.add_argument("--min-leaf", type=int, default=1)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        if REVISION.fullmatch(arguments.generator_revision) is None:
            raise ValueError("generator revision must be 40 lowercase hexadecimal characters")
        if IDENTIFIER.fullmatch(arguments.resource_id) is None:
            raise ValueError("resource ID is not canonical")
        if IDENTIFIER.fullmatch(arguments.split_seed) is None:
            raise ValueError("split seed is not canonical")
        require_int(arguments.heldout_percent, 1, 50, "held-out percent")
        require_int(arguments.context_left, 0, MAX_CONTEXT, "left context")
        require_int(arguments.context_right, 0, MAX_CONTEXT, "right context")
        if arguments.context_left == 0 and arguments.context_right == 0:
            raise ValueError("at least one context direction must be enabled")
        require_int(arguments.max_depth, 1, MAX_DEPTH, "maximum tree depth")
        require_int(arguments.min_leaf, 1, 1000, "minimum leaf examples")
        input_data = read_verified(arguments.input, arguments.input_sha256)
        corpus = parse_corpus(input_data, arguments.required_admission)
        evidence = (
            arguments.source_lexicon,
            arguments.alignment_record,
            arguments.license_record,
            arguments.review_record,
        )
        if corpus.admission == "product-admitted":
            if any(path is None for path in evidence):
                raise ValueError(
                    "product admission requires source lexicon, alignment, "
                    "license, and review records")
            read_verified(arguments.source_lexicon,
                          corpus.source_lexicon_sha256,
                          "source lexicon")
            read_verified(arguments.alignment_record,
                          corpus.alignment_record_sha256,
                          "alignment record", 16 * 1024 * 1024)
            read_verified(arguments.license_record,
                          corpus.license_record_sha256,
                          "license record", 16 * 1024 * 1024)
            read_verified(arguments.review_record,
                          corpus.review_record_sha256,
                          "review record", 16 * 1024 * 1024)
        elif any(path is not None for path in evidence):
            raise ValueError(
                "non-product training must not receive product evidence files")
        model_data, training_data, manifest_data = train(
            corpus, input_data, arguments.input_sha256,
            arguments.generator_revision, arguments.resource_id,
            arguments.split_seed, arguments.heldout_percent,
            arguments.context_left, arguments.context_right,
            arguments.max_depth, arguments.min_leaf)
        write_output(arguments.output_dir, model_data, training_data,
                     manifest_data)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
