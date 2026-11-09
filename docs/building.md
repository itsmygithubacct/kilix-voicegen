# Building, testing and the fixture CLI

Configure, build and test with CMake presets, enable the optional ONNX Runtime backend, and exercise the fixture package. Moved from the README.

## Build and test

An out-of-tree CMake build needs a C++17 compiler, Python 3.11 or newer, and a
clean checkout of utf8proc at the exact revision in
`cmake/dependencies.lock.json`. Point CMake at that source checkout; utf8proc is
built statically. The default build has no neural-runtime dependency; in it
the Piper interoperability test `piper_pilot_interop` is still registered and
`ctest` reports it as skipped, naming each unmet precondition, so the total
stays at 21 and the gate says which arm did not run.

```sh
export KGV_UTF8PROC_SOURCE_DIR=/path/to/utf8proc
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

To enable the Linux research backend, also provide the clean pinned ONNX Runtime
source checkout (for the exact C API headers and notices) and the exact external
CPU shared library recorded in the dependency lock:

```sh
cmake -S . -B /path/to/build \
  -DKGV_UTF8PROC_SOURCE_DIR=/path/to/utf8proc \
  -DKGV_ENABLE_ONNXRUNTIME=ON \
  -DKGV_ONNXRUNTIME_SOURCE_DIR=/path/to/onnxruntime \
  -DKGV_ONNXRUNTIME_LIBRARY=/path/to/libonnxruntime.so.1.29.0
cmake --build /path/to/build
ctest --test-dir /path/to/build --output-on-failure
```

CMake verifies the source revisions, clean worktrees, exact library filename,
and library SHA-256. The build and install place the library beside
`libkilix_voicegen`, and the runtime asks for that exact sibling filename. It
does not download a runtime or model. Supplying
`KGV_PIPER_PILOT_MODEL_DIR` and `KGV_PIPER_PILOT_RELEASE_SHA` at configure time
adds the external end-to-end speech, repeatability, voice/seed, and cancellation
test without copying the package into the source tree.

Inspect the current lexical slice without a model:

```sh
kilix-voicegen frontend --profile prose --text 'Meet at 09:05 on 2026-08-25.'
printf '%s\n' '127.0.0.1:8080 --no-cache' |
  kilix-voicegen frontend --profile terminal --stdin
```

The JSON trace is diagnostic output for development. It contains normalized
words and source spans, but not yet reviewed pronunciations or model-token IDs.

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
WAV output. A fixture package produces a test tone; a verified
`piper-vits-onnx/v1` package produces speech in an ONNX-enabled build. Generated
audio and all model packages must stay outside this repository.
