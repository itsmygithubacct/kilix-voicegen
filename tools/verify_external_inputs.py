#!/usr/bin/env python3
"""Verify caller-supplied benchmark inputs against a repository lock file."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
from typing import Any


class InputError(RuntimeError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InputError("lock file contains a duplicate object key")
        result[key] = value
    return result


def verify(root: pathlib.Path, lock_path: pathlib.Path) -> dict[str, Any]:
    with lock_path.open(encoding="utf-8") as stream:
        lock = json.load(stream, object_pairs_hook=_strict_object)
    if lock.get("schema") != "kilix.voicegen.benchmark-inputs/v1":
        raise InputError("benchmark input lock schema is unsupported")
    inputs = lock.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        raise InputError("benchmark input lock has no inputs")

    checked = 0
    for entry in inputs:
        if not isinstance(entry, dict):
            raise InputError("benchmark input entry is invalid")
        relative = entry.get("source_name")
        if (not isinstance(relative, str) or not relative or relative.startswith("/")
                or "\\" in relative or ".." in relative.split("/")):
            raise InputError("benchmark input name is not a safe relative path")
        path = root.joinpath(*relative.split("/"))
        if path.is_symlink() or not path.is_file():
            raise InputError("required benchmark input is absent or not a regular file")
        data = path.read_bytes()
        if len(data) != entry.get("bytes") or hashlib.sha256(data).hexdigest() != entry.get("sha256"):
            raise InputError("benchmark input byte count or SHA-256 does not match lock")
        if "records" in entry:
            records = sum(1 for line in data.splitlines() if line.strip())
            if records != entry["records"]:
                raise InputError("benchmark corpus record count does not match lock")
        checked += 1
    return {"schema": "kilix.voicegen.benchmark-verification/v1",
            "status": "ok", "inputs": checked}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--lock", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent
                        / "benchmarks" / "baseline.lock.json")
    arguments = parser.parse_args()
    try:
        result = verify(arguments.root, arguments.lock)
    except InputError as error:
        print(json.dumps({"schema": "kilix.voicegen.benchmark-verification/v1",
                          "status": "error", "detail": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
