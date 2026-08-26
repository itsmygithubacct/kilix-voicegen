#!/usr/bin/env python3
"""Black-box tests for the deterministic compact LTS trainer."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import string
import subprocess
import tempfile
import unittest
from typing import Any


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key {key}")
        result[key] = value
    return result


def canonical(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


class LtsTrainerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = os.environ["KGV_LTS_TRAINER"]
        cls.validator = os.environ["KGV_LTS_VALIDATOR"]

    @staticmethod
    def sha(data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    @staticmethod
    def corpus(*, review: str | None = None,
               admission: str = "test-fixture") -> bytes:
        inventory = [
            {"id": 1, "name": "AO"},
            {"id": 2, "name": "AE"},
            {"id": 3, "name": "B"},
            {"id": 4, "name": "D"},
            {"id": 5, "name": "K"},
            {"id": 6, "name": "M"},
            {"id": 7, "name": "N"},
            {"id": 8, "name": "P"},
            {"id": 9, "name": "T"},
        ]
        inventory_bytes = b"".join(
            f"{entry['id']}\t{entry['name']}\n".encode("ascii")
            for entry in inventory
        )
        words = sorted(
            first + "a" + last
            for first in "bct"
            for last in "bdmnpt"
        )
        header = {
            "admission": admission,
            "alignment_record_sha256": "3" * 64,
            "dialect": "en-AU",
            "entry_count": len(words),
            "license_record_sha256": "4" * 64,
            "review_record_sha256": review,
            "schema": "kilix.voicegen.lts-aligned-corpus/v1",
            "segment_inventory": inventory,
            "segment_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
            "source_lexicon_sha256": "5" * 64,
        }
        segment = {"b": 3, "c": 5, "d": 4, "m": 6, "n": 7,
                   "p": 8, "t": 9}
        documents = [header]
        for word in words:
            documents.append({
                "emissions": [
                    [{"segment_ids": [segment[word[0]]], "syllable_end": None}],
                    [{"segment_ids": [2 if word[-1] == "t" else 1],
                      "syllable_end": None}],
                    [{"segment_ids": [segment[word[-1]]],
                      "syllable_end": "primary"}],
                ],
                "schema": "kilix.voicegen.lts-aligned-entry/v1",
                "word": word,
            })
        return b"".join(canonical(document) for document in documents)

    @staticmethod
    def product_corpus(evidence: dict[str, bytes]) -> bytes:
        inventory = [
            {"id": 1, "name": "AO"},
            {"id": 2, "name": "AE"},
            {"id": 3, "name": "B"},
            {"id": 4, "name": "D"},
            {"id": 5, "name": "K"},
            {"id": 6, "name": "M"},
            {"id": 7, "name": "N"},
            {"id": 8, "name": "P"},
            {"id": 9, "name": "T"},
        ]
        inventory_bytes = b"".join(
            f"{entry['id']}\t{entry['name']}\n".encode("ascii")
            for entry in inventory
        )
        words = [first + last for first in string.ascii_lowercase
                 for last in string.ascii_lowercase]
        header = {
            "admission": "product-admitted",
            "alignment_record_sha256": hashlib.sha256(
                evidence["alignment-record"]).hexdigest(),
            "dialect": "en-AU",
            "entry_count": len(words),
            "license_record_sha256": hashlib.sha256(
                evidence["license-record"]).hexdigest(),
            "review_record_sha256": hashlib.sha256(
                evidence["review-record"]).hexdigest(),
            "schema": "kilix.voicegen.lts-aligned-corpus/v1",
            "segment_inventory": inventory,
            "segment_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
            "source_lexicon_sha256": hashlib.sha256(
                evidence["source-lexicon"]).hexdigest(),
        }
        documents = [header]
        documents.extend({
            "emissions": [
                [{"segment_ids": [5], "syllable_end": None}],
                [{"segment_ids": [9], "syllable_end": "primary"}],
            ],
            "schema": "kilix.voicegen.lts-aligned-entry/v1",
            "word": word,
        } for word in words)
        return b"".join(canonical(document) for document in documents)

    def invoke(self, root: pathlib.Path, data: bytes, *, name: str,
               expected_sha: str | None = None,
               admission: str = "test-fixture",
               evidence: dict[str, bytes] | None = None
               ) -> subprocess.CompletedProcess[bytes]:
        source = root / f"{name}.jsonl"
        output = root / f"{name}.output"
        source.write_bytes(data)
        command = [
            self.tool,
            "--input", os.fspath(source),
            "--input-sha256", expected_sha or self.sha(data),
            "--required-admission", admission,
            "--generator-revision", "6" * 40,
            "--resource-id", "kilix-en-au-lts-test-1",
            "--split-seed", "kilix-en-au-lts-test-split-1",
            "--heldout-percent", "25",
            "--context-left", "1",
            "--context-right", "1",
            "--max-depth", "8",
            "--min-leaf", "1",
            "--output-dir", os.fspath(output),
        ]
        if evidence is not None:
            for name_part in ("source-lexicon", "alignment-record",
                              "license-record", "review-record"):
                evidence_path = root / f"{name}.{name_part}"
                evidence_path.write_bytes(evidence[name_part])
                command.extend([f"--{name_part}", os.fspath(evidence_path)])
        return subprocess.run(command, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
            timeout=20)

    def test_training_is_deterministic_hash_bound_and_measured(self) -> None:
        data = self.corpus()
        with tempfile.TemporaryDirectory(prefix="kgv-lts-trainer-") as raw:
            root = pathlib.Path(raw)
            first = self.invoke(root, data, name="first")
            second = self.invoke(root, data, name="second")
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            first_dir = root / "first.output"
            second_dir = root / "second.output"
            for name in ("model.jsonl", "TRAINING.json", "MANIFEST.json"):
                self.assertEqual((first_dir / name).read_bytes(),
                                 (second_dir / name).read_bytes())

            training_data = (first_dir / "TRAINING.json").read_bytes()
            training = json.loads(training_data, object_pairs_hook=strict_object)
            model_data = (first_dir / "model.jsonl").read_bytes()
            model_lines = model_data.splitlines()
            model_header = json.loads(model_lines[0], object_pairs_hook=strict_object)
            nodes = [json.loads(line, object_pairs_hook=strict_object)
                     for line in model_lines[1:]]
            manifest = json.loads((first_dir / "MANIFEST.json").read_bytes(),
                                  object_pairs_hook=strict_object)

            self.assertEqual(training["aligned_corpus"]["sha256"], self.sha(data))
            self.assertEqual(training["aligned_corpus"]["entries"], 18)
            self.assertGreater(training["metrics"]["train"]["words"], 0)
            self.assertGreater(training["metrics"]["heldout"]["words"], 0)
            self.assertEqual(
                training["metrics"]["train"]["words"] +
                training["metrics"]["heldout"]["words"], 18)
            self.assertEqual(training["metrics"]["train"]["invalid_emission_words"],
                             0)
            self.assertEqual(
                training["metrics"]["heldout"]["invalid_emission_words"], 0)
            self.assertEqual(model_header["training_record_sha256"],
                             self.sha(training_data))
            self.assertEqual(model_header["node_count"], len(nodes))
            self.assertEqual([node["id"] for node in nodes], list(range(len(nodes))))
            self.assertEqual(manifest["files"]["model.jsonl"]["sha256"],
                             self.sha(model_data))
            self.assertEqual(manifest["files"]["TRAINING.json"]["sha256"],
                             self.sha(training_data))
            self.assertEqual(training["status"], "non-product-training-fixture")
            self.assertIsNone(model_header["review_record_sha256"])
            validation = subprocess.run(
                [self.validator, "--validate", os.fspath(first_dir / "model.jsonl")],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                timeout=10)
            self.assertEqual(validation.returncode, 0, validation.stderr)

    def test_hash_admission_canonical_and_review_fail_closed(self) -> None:
        data = self.corpus()
        with tempfile.TemporaryDirectory(prefix="kgv-lts-trainer-") as raw:
            root = pathlib.Path(raw)
            mismatch = self.invoke(root, data, name="hash",
                                   expected_sha="0" * 64)
            self.assertEqual(mismatch.returncode, 1)
            self.assertIn(b"SHA-256 mismatch", mismatch.stderr)
            self.assertFalse((root / "hash.output").exists())

            admission = self.invoke(root, data, name="admission",
                                    admission="product-admitted")
            self.assertEqual(admission.returncode, 1)
            self.assertIn(b"admission", admission.stderr)
            self.assertFalse((root / "admission.output").exists())

            product_data = self.corpus(review="a" * 64,
                                       admission="product-admitted")
            missing_evidence = self.invoke(
                root, product_data, name="missing-evidence",
                admission="product-admitted")
            self.assertEqual(missing_evidence.returncode, 1)
            self.assertIn(b"requires source lexicon", missing_evidence.stderr)
            self.assertFalse((root / "missing-evidence.output").exists())

            false_review_data = self.corpus(review="a" * 64)
            false_review = self.invoke(root, false_review_data,
                                       name="false-review")
            self.assertEqual(false_review.returncode, 1)
            self.assertIn(b"must not claim review", false_review.stderr)
            self.assertFalse((root / "false-review.output").exists())

            first_line, remainder = data.split(b"\n", 1)
            noncanonical_data = b" " + first_line + b"\n" + remainder
            noncanonical = self.invoke(root, noncanonical_data,
                                       name="noncanonical")
            self.assertEqual(noncanonical.returncode, 1)
            self.assertIn(b"not in canonical form", noncanonical.stderr)
            self.assertFalse((root / "noncanonical.output").exists())

    def test_existing_output_is_never_overwritten(self) -> None:
        data = self.corpus()
        with tempfile.TemporaryDirectory(prefix="kgv-lts-trainer-") as raw:
            root = pathlib.Path(raw)
            first = self.invoke(root, data, name="same")
            self.assertEqual(first.returncode, 0, first.stderr)
            marker = root / "same.output" / "marker"
            marker.write_text("retain", encoding="utf-8")
            second = self.invoke(root, data, name="same")
            self.assertEqual(second.returncode, 1)
            self.assertEqual(marker.read_text(encoding="utf-8"), "retain")

    def test_product_mode_requires_verified_evidence_and_complete_roots(self) -> None:
        evidence = {
            "source-lexicon": b"First-party product-mode test lexicon.\n",
            "alignment-record": b"First-party alignment test record.\n",
            "license-record": b"First-party test license record.\n",
            "review-record": b"First-party dual-review test record.\n",
        }
        data = self.product_corpus(evidence)
        with tempfile.TemporaryDirectory(prefix="kgv-lts-trainer-") as raw:
            root = pathlib.Path(raw)
            missing = self.invoke(root, data, name="missing",
                                  admission="product-admitted")
            self.assertEqual(missing.returncode, 1)
            self.assertIn(b"requires source lexicon", missing.stderr)

            accepted = self.invoke(root, data, name="accepted",
                                   admission="product-admitted",
                                   evidence=evidence)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            output = root / "accepted.output"
            training = json.loads((output / "TRAINING.json").read_bytes(),
                                  object_pairs_hook=strict_object)
            header = json.loads((output / "model.jsonl").read_bytes().splitlines()[0],
                                object_pairs_hook=strict_object)
            self.assertEqual(training["status"], "product-admitted")
            self.assertEqual(set(header["roots"]), set(string.ascii_lowercase))
            self.assertEqual(training["metrics"]["heldout"]["unresolved_words"],
                             0)
            validation = subprocess.run(
                [self.validator, "--validate-product",
                 os.fspath(output / "model.jsonl")],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                timeout=10)
            self.assertEqual(validation.returncode, 0, validation.stderr)


if __name__ == "__main__":
    unittest.main()
