#!/usr/bin/env python3
"""Build an explicitly unreviewed en-AU lexicon candidate from pinned CMUdict."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import tempfile
from typing import Any

SCHEMA = "kilix.voicegen.en-au-lexicon-candidate/v1"
MANIFEST_SCHEMA = "kilix.voicegen.en-au-lexicon-candidate-manifest/v1"
POLICY_ID = "kilix-en-au-cmudict-transform-1"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
ENTRY = re.compile(r"(?P<word>\S+?)(?:\((?P<variant>[2-9][0-9]*)\))?\Z")

VOWELS = {"AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH",
          "IY", "OW", "OY", "UH", "UW"}
CONSONANTS = {"B", "CH", "D", "DH", "F", "G", "HH", "JH", "K", "L", "M",
              "N", "NG", "P", "R", "S", "SH", "T", "TH", "V", "W", "Y",
              "Z", "ZH"}
PHONE_MAP = {
    "AE": "AE",
    "AO": "AO_LONG",
    "AW": "AW",
    "AY": "AY",
    "EH": "EH",
    "EY": "EY",
    "IH": "IH",
    "IY": "IY_LONG",
    "OW": "OW",
    "OY": "OY",
    "UH": "UH",
    "UW": "UW_LONG",
}
POLICY = {
    "id": POLICY_ID,
    "source_phones": sorted(VOWELS | CONSONANTS),
    "direct_vowel_map": PHONE_MAP,
    "unstressed_ah": "AX",
    "stressed_ah": "AH_STRUT",
    "unstressed_er": "AX",
    "stressed_er": "ER_NURSE",
    "ambiguous_aa": "AA_LOT_PALM_REVIEW",
    "non_rhotic_rule": "drop vowel-following R unless immediately followed by a vowel",
    "review_flags": [
        "AA_LOT_PALM_SPLIT_REVIEW",
        "AE_TRAP_BATH_SPLIT_REVIEW",
        "NO_PRONOUNCED_VOWEL_REVIEW",
        "POSTVOCALIC_R_DROPPED",
        "RHOTIC_VOWEL_TRANSFORMED",
    ],
}

JsonObject = dict[str, Any]


def canonical(document: JsonObject) -> bytes:
    return (json.dumps(document, ensure_ascii=True, separators=(",", ":"),
                       sort_keys=True) + "\n").encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_verified(path: pathlib.Path, expected_sha256: str) -> bytes:
    if not SHA256.fullmatch(expected_sha256):
        raise ValueError("expected SHA-256 must be 64 lowercase hexadecimal characters")
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"input is absent, non-regular, or symbolic: {path.name}")
    data = path.read_bytes()
    actual = digest(data)
    if actual != expected_sha256:
        raise ValueError(f"SHA-256 mismatch for {path.name}: expected {expected_sha256}, got {actual}")
    return data


def parse_phone(raw: str, *, line_number: int) -> tuple[str, int | None]:
    base = raw.rstrip("012")
    stress_text = raw[len(base):]
    if base in VOWELS:
        if len(stress_text) != 1:
            raise ValueError(f"line {line_number}: vowel {raw!r} lacks exactly one stress digit")
        return base, int(stress_text)
    if base in CONSONANTS and not stress_text:
        return base, None
    raise ValueError(f"line {line_number}: unknown or malformed CMU phone {raw!r}")


def transform(phones: list[tuple[str, int | None]]) -> tuple[list[str], list[int], list[str]]:
    segments: list[str] = []
    stresses: list[int] = []
    flags: set[str] = set()
    for index, (phone, stress) in enumerate(phones):
        if phone == "R":
            previous_is_vowel = index > 0 and phones[index - 1][0] in VOWELS
            next_is_vowel = index + 1 < len(phones) and phones[index + 1][0] in VOWELS
            if previous_is_vowel and not next_is_vowel:
                flags.add("POSTVOCALIC_R_DROPPED")
                continue
            segments.append("R")
            stresses.append(-1)
            continue
        if phone in CONSONANTS:
            segments.append(phone)
            stresses.append(-1)
            continue
        assert stress is not None
        if phone == "AA":
            segment = "AA_LOT_PALM_REVIEW"
            flags.add("AA_LOT_PALM_SPLIT_REVIEW")
        elif phone == "AE":
            segment = PHONE_MAP[phone]
            flags.add("AE_TRAP_BATH_SPLIT_REVIEW")
        elif phone == "AH":
            segment = "AX" if stress == 0 else "AH_STRUT"
        elif phone == "ER":
            segment = "AX" if stress == 0 else "ER_NURSE"
            flags.add("RHOTIC_VOWEL_TRANSFORMED")
        else:
            segment = PHONE_MAP[phone]
        segments.append(segment)
        stresses.append(stress)
    if not segments:
        raise ValueError("transformed entry has no pronounced segment")
    if not any(stress >= 0 for stress in stresses):
        # CMUdict deliberately contains a small number of consonant-only
        # interjections (for example "hm" and "shh").  Preserve them, but
        # quarantine them from automatic admission because a later phone set
        # must decide whether a consonant is syllabic and how it is stressed.
        flags.add("NO_PRONOUNCED_VOWEL_REVIEW")
    return segments, stresses, sorted(flags)


def parse_dictionary(data: bytes) -> list[JsonObject]:
    try:
        text = data.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        raise ValueError(f"CMUdict input is not strict UTF-8: {error}") from error
    records: list[JsonObject] = []
    seen: set[tuple[str, int]] = set()
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        if not raw_line or raw_line.startswith(";;;"):
            continue
        entry_line, separator, _comment = raw_line.partition(" # ")
        if raw_line != raw_line.strip() or "  " in entry_line or "\t" in entry_line:
            raise ValueError(f"line {line_number}: entry must use canonical single-space fields")
        if "#" in entry_line or (separator and not _comment):
            raise ValueError(f"line {line_number}: malformed inline comment")
        fields = entry_line.split(" ")
        if len(fields) < 2:
            raise ValueError(f"line {line_number}: entry has no pronunciation")
        matched = ENTRY.fullmatch(fields[0])
        if matched is None:
            raise ValueError(f"line {line_number}: malformed grapheme or variant")
        grapheme = matched.group("word")
        variant = int(matched.group("variant") or "1") - 1
        key = (grapheme, variant)
        if key in seen:
            raise ValueError(f"line {line_number}: duplicate grapheme/variant")
        seen.add(key)
        phones = [parse_phone(raw, line_number=line_number) for raw in fields[1:]]
        segments, stresses, flags = transform(phones)
        records.append({
            "candidate_segments": segments,
            "grapheme": grapheme,
            "review_flags": flags,
            "review_status": "needs-review" if flags else "machine-candidate-unreviewed",
            "schema": SCHEMA,
            "source_line": line_number,
            "source_phones": fields[1:],
            "stresses": stresses,
            "variant": variant,
        })
    if not records:
        raise ValueError("CMUdict input contains no entries")
    records.sort(key=lambda item: (item["grapheme"], item["variant"]))
    return records


def write_candidate(output: pathlib.Path, records: list[JsonObject], source: JsonObject,
                    generator: JsonObject, license_data: bytes) -> None:
    if output.exists() or output.is_symlink():
        raise ValueError("output directory already exists; refusing to overwrite it")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        candidate_data = b"".join(canonical(record) for record in records)
        review_records = [record for record in records if record["review_flags"]]
        review_data = b"".join(canonical(record) for record in review_records)
        policy_data = canonical(POLICY)
        files = {
            "CMUDICT_LICENSE": license_data,
            "POLICY.json": policy_data,
            "candidate.jsonl": candidate_data,
            "review-queue.jsonl": review_data,
        }
        for name, data in files.items():
            (temporary / name).write_bytes(data)
        flag_counts: dict[str, int] = {}
        for record in review_records:
            for flag in record["review_flags"]:
                flag_counts[flag] = flag_counts.get(flag, 0) + 1
        manifest = {
            "candidate_entries": len(records),
            "files": {
                name: {"bytes": len(data), "sha256": digest(data)}
                for name, data in sorted(files.items())
            },
            "generator": generator,
            "policy_id": POLICY_ID,
            "policy_sha256": digest(policy_data),
            "review_flag_counts": dict(sorted(flag_counts.items())),
            "review_queue_entries": len(review_records),
            "schema": MANIFEST_SCHEMA,
            "source": source,
            "status": "machine-transformed-unreviewed-not-product-admitted",
        }
        (temporary / "MANIFEST.json").write_bytes(canonical(manifest))
        os.replace(temporary, output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--generator-revision", required=True)
    parser.add_argument("--source-license", type=pathlib.Path, required=True)
    parser.add_argument("--source-license-sha256", required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", arguments.source_revision):
        parser.error("--source-revision must be an exact 40-character lowercase Git commit")
    if not re.fullmatch(r"[0-9a-f]{40}", arguments.generator_revision):
        parser.error("--generator-revision must be an exact 40-character lowercase Git commit")
    try:
        input_data = read_verified(arguments.input, arguments.input_sha256)
        license_data = read_verified(arguments.source_license,
                                     arguments.source_license_sha256)
        records = parse_dictionary(input_data)
        source = {
            "dictionary_bytes": len(input_data),
            "dictionary_sha256": arguments.input_sha256,
            "license_bytes": len(license_data),
            "license_sha256": arguments.source_license_sha256,
            "repository": "https://github.com/cmusphinx/cmudict.git",
            "revision": arguments.source_revision,
        }
        generator_path = pathlib.Path(__file__).resolve()
        generator_data = generator_path.read_bytes()
        generator = {
            "repository": "https://github.com/itsmygithubacct/kilix-voicegen.git",
            "revision": arguments.generator_revision,
            "sha256": digest(generator_data),
        }
        write_candidate(arguments.output_dir, records, source, generator, license_data)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
