# Kilix Voicegen

Kilix Voicegen is a private, offline English text-to-speech engine under active
development. Its v1 product target is one original female-presenting voice and
one original male-presenting voice, both speaking General Australian English,
at or above the selected Piper quality and CPU-operability floor.

**Status:** in development (Phase 2, milestone P1 active); there is no
release-approved voice yet. Not part of Plebian-OS. The Kilix for Windows
preview's WSL app catalog (`windows/wsl/app-catalog.json` in kilix) lists it as
"Voicegen developer tools": the native lexical frontend and package
verification, with neural research synthesis excluded and no models or
acceptance receipts downloaded. This repository contains no model, recording,
checkpoint or generated audio.

## What it does

- A C++17 library (`libkilix_voicegen`) with a frozen C ABI
  (`include/kilix_voicegen.h`), a Python binding, and the `kilix-voicegen` CLI.
- A typed `en-AU` lexical frontend: strict UTF-8, NFC through pinned utf8proc,
  `prose` and `terminal` normalization profiles, and a JSON diagnostic trace.
- Strict, hash-chained model-package verification before an engine opens.
- A deterministic fixture engine that emits a test tone, so streaming,
  backpressure and cancellation can be tested without a neural model.
- An optional `piper-vits-onnx/v1` research backend on a hash-pinned ONNX
  Runtime 1.29.0 library, which produces speech from a compatible external
  package.

**This engine can now produce speech when built with the optional ONNX backend
and opened with a compatible external package.** The clean source tree still
contains no model, recording, checkpoint, or generated audio. The current
Australian-female package is an internal technical pilot, not the
release-approved `kilix-female-01`: its recording and warm-start lineage do not
satisfy the production consent/provenance gate.

## Quick start

Point CMake at a clean utf8proc checkout at the revision in
`cmake/dependencies.lock.json`, then build and test with the `dev` preset:

```sh
export KGV_UTF8PROC_SOURCE_DIR=/path/to/utf8proc
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Inspect the lexical frontend without a model:

```sh
kilix-voicegen frontend --profile prose --text 'Meet at 09:05 on 2026-08-25.'
```

The ONNX Runtime research build, the sanitizer preset and installation are in
[docs/building.md](docs/building.md).

## Usage

| Command | What it does |
|---|---|
| `kilix-voicegen --version` / `--abi-version` | Print the library and ABI versions. |
| `kilix-voicegen frontend --profile prose\|terminal (--text T \| --stdin)` | Print the JSON lexical trace. |
| `kilix-voicegen verify --model DIR --release-sha HEX` | Verify a model package against its pinned release hash. |
| `kilix-voicegen synthesize --model DIR --release-sha HEX --voice kilix-female-01\|kilix-male-01 --profile P (--text T \| --stdin) --output F.wav [--rate 0.70..1.50] [--seed N]` | Write 24 kHz mono s16 WAV. |
| `cmake --preset dev\|asan-ubsan\|release` | Configure a build outside the tree. |

## How it works

```
UTF-8 text -> validation (64 KiB, prose|terminal) -> NFC + tokenization
   -> overrides -> local dictionary -> lexicon (+ heteronym roles)
   -> morphology -> letter-to-sound forest -> weak forms
   -> model-token packing (bounded chunks)
   -> engine: fixture tone | piper-vits-onnx/v1 (ONNX Runtime 1.29.0)
   -> 20 ms PCM callbacks at 24 kHz, with backpressure and cancellation
```

A caller opens an engine with a model directory and the SHA-256 of its
`RELEASE.json`; the engine verifies the manifest and every payload role
(segment inventory, lexicon, LTS forest, model-token inventory) before
any job can run. Each job resolves the text through the frontend resources in a
fixed order, packs model tokens into bounded chunks, and streams PCM through a
synchronous callback. The Piper research backend projects the Kilix token ABI
into Piper's ID space and resamples 22.05 kHz output to 24 kHz. The ABI and
every resource format are in [docs/runtime-contract.md](docs/runtime-contract.md);
machine-readable schemas are in [schemas/](schemas/) and conformance vectors in
[tests/conformance/](tests/conformance/).

## Documentation

- [docs/status.md](docs/status.md): the current milestone, what exists and
  which review gates remain open.
- [docs/building.md](docs/building.md): build and test, the ONNX Runtime
  research backend, the sanitizer gate, installation and the fixture CLI.
- [docs/runtime-contract.md](docs/runtime-contract.md): the v1 runtime contract
  and the pronunciation, LTS, model-token, heteronym, morphology and weak-form
  resources.
- [THIRD_PARTY.md](THIRD_PARTY.md): the third-party boundary (utf8proc, ONNX
  Runtime, the external pilot package).
- [benchmarks/README.md](benchmarks/README.md): the benchmark input lock.
- [tests/fixtures/README.md](tests/fixtures/README.md): the first-party test
  fixtures.

## Related repositories

- [kilix-piper-tts](https://github.com/itsmygithubacct/kilix-piper-tts#readme):
  the Piper speech provider Kilix Voice uses today.
- [kilix-voice](https://github.com/itsmygithubacct/kilix-voice#readme): Kilix
  read-aloud and dictation.
- [kilix](https://github.com/itsmygithubacct/kilix#readme): carries the Windows
  preview catalog entry for this engine.

## Part of Plebian-OS

Kilix Voicegen is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It is in development and does not ship in Plebian-OS; only the Kilix for
Windows preview lists its developer tools. The map of every repository and how
it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE)
(Copyright (c) 2026 Kilix Voicegen contributors). Third-party dependencies are
not distributed here; see [THIRD_PARTY.md](THIRD_PARTY.md).
