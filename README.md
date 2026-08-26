# Kilix Voicegen

Kilix Voicegen is a private, offline English text-to-speech engine under active
development. Its v1 product target is one original female-presenting voice and
one original male-presenting voice at or above the selected Piper quality and
CPU-operability floor.

## Current status

Phase 2 foundation is in progress. The repository currently provides the
frozen C ABI draft, strict model-package verification, the first UTF-8/control
validation slice, a native CLI, and a deterministic fixture engine. The fixture
emits a quiet triangle-wave test signal so streaming, backpressure,
cancellation, corruption handling, and language bindings can be tested without
a model.

**This snapshot does not yet produce speech.** It contains no trained model,
recorded voice, downloaded checkpoint, or generated audio.

## Build and test

An out-of-tree CMake build needs a C++17 compiler and Python 3.11 or newer. The
foundation build has no network or third-party package requirement.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The sanitizer gate is:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

Build directories live outside the source tree by preset. To install into a
staging prefix, configure with `-DCMAKE_INSTALL_PREFIX=/path` and build the
`install` target.

## Fixture CLI

The developer preset generates a hash-chained fixture package outside this
tree and records its outer hash in the package directory. Verify it with:

```sh
kgv_build=../.build/kilix-voicegen-dev
"$kgv_build/kilix-voicegen" verify \
  --model "$kgv_build/fixture-model" \
  --release-sha "$(tr -d '\n' < "$kgv_build/fixture-model/RELEASE.sha256")"
```

`kilix-voicegen synthesize` exercises the same ABI and writes 24 kHz mono s16
WAV output. With the current fixture package, that output is a test tone, never
speech. Generated audio and model packages must stay outside this repository.

## Runtime contract

- strict UTF-8 input, explicit `prose` or `terminal` profile, and a 64 KiB hard
  request ceiling;
- exactly `kilix-female-01` and `kilix-male-01` in the v1 model contract;
- synchronous 20 ms PCM callbacks at 24 kHz, with consumer backpressure and
  thread-safe cancellation;
- caller-pinned `RELEASE.json` hash followed by a verified manifest and payload
  chain;
- fail-closed schema, ABI, tensor, CPU-feature, byte-count, and hash checks;
- no network, audio-device, reference-audio, cloning, SSML, or prompt API.

Machine-readable schemas live in `schemas/`; executable conformance vectors
live in `tests/conformance/`. Research notes, source snapshots, recordings,
models, experiments, benchmark output, and implementation planning remain
outside this source repository.
