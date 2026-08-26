# Kilix Voicegen

Kilix Voicegen is a private, offline English text-to-speech engine under active
development. Its v1 product target is one original female-presenting voice and
one original male-presenting voice, both speaking General Australian English,
at or above the selected Piper quality and CPU-operability floor.

## Current status

Phase 2 milestone P0 (foundation and frozen contracts) is complete. The
repository provides the frozen C ABI draft, strict model-package verification,
strict UTF-8/control validation, a typed lexical frontend trace, a native CLI,
a deterministic fixture engine, and an optional native Piper/VITS ONNX research
backend. P1 implementation is active: the pinned utf8proc/Unicode 17 dependency
supplies NFC with original-byte spans, and the staged lexical/normalization
vectors execute. Reviewed product pronunciation data, production recordings,
and release models remain open. A strict pronunciation-resource ABI and native
loader now
bind canonical `en-AU` JSONL
to caller-pinned file and segment-inventory hashes, enforce bounded NFC entries
and role-qualified syllables, and require a separate review-record hash before a
resource can be admitted as product data. No product lexicon has yet passed that
human review gate. A compact deterministic LTS decision-forest trainer and
native inference loader now provide the corresponding offline fallback
mechanism, but only first-party in-memory/test data have exercised it; no
product LTS artifact exists. Model packages now carry the segment inventory,
lexicon, LTS forest, and model-token inventory as distinct hashed roles. Engine
open loads those exact verified bytes and validates the complete admission/hash
chain; every public job resolves and stores bounded model-token chunks before it
can run. The fixture emits a quiet triangle-wave test signal so streaming,
backpressure, cancellation, corruption handling, and language bindings can be
tested before the neural backend is installed.

The optional `piper-vits-onnx/v1` path loads an exact hash-pinned ONNX Runtime
1.29.0 shared library, creates a session from the already verified in-memory
graph bytes, checks the graph's real tensor names/types/shapes, projects the
resolved Kilix token ABI into the Piper ID space, and resamples 22.05 kHz graph
output to the fixed 24 kHz ABI. It preserves synchronous 480-frame callbacks
and uses ONNX Runtime termination for cancellation during inference. The public
C ABI is unchanged.

The v1 frontend and model contract is fixed to `en-AU`; it has no automatic
dialect chooser. British English, then General American English, are
whole-release recruitment/licensing fallbacks that require a new contract.

**This engine can now produce speech when built with the optional ONNX backend
and opened with a compatible external package.** The clean source tree still
contains no model, recording, checkpoint, or generated audio. The current
Australian-female package is an internal technical pilot, not the
release-approved `kilix-female-01`: its recording and warm-start lineage do not
satisfy the production consent/provenance gate.

## Build and test

An out-of-tree CMake build needs a C++17 compiler, Python 3.11 or newer, and a
clean checkout of utf8proc at the exact revision in
`cmake/dependencies.lock.json`. Point CMake at that source checkout; utf8proc is
built statically. The default build has no neural-runtime dependency.

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

## Runtime contract

- strict UTF-8 input, explicit `prose` or `terminal` profile, and a 64 KiB hard
  request ceiling;
- one implicit General Australian English (`en-AU`) frontend shared by both
  voices;
- only the fixed IDs `kilix-female-01` and `kilix-male-01`; a package may carry
  one during a bounded pilot or both for the product release;
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

The pronunciation JSONL contract has separate strict header and entry schemas.
Its loader accepts resource bytes rather than discovering files, verifies the
whole-resource SHA-256 and canonical segment-ID inventory, keeps exact and
ASCII-folded lookups separate, resolves role-qualified variants before defaults,
and clears all loaded state on every validation failure.

The compact LTS JSONL contract likewise binds the complete forest, canonical
segment inventory, source lexicon, training record, and (for product admission)
the separate review record. Its bounded native evaluator reconstructs segment,
syllable, and stress records without filesystem or network discovery. The
standard-library-only `tools/train_lts_cart.py` trains deterministic
context-window CART-style forests from explicitly hash-pinned, letter-aligned
JSONL; it records a SHA-256 word split and held-out exact-symbol, exact-word,
and segment-edit metrics. Product mode additionally requires the exact source
lexicon, alignment, license, and review evidence files—not only their claimed
hash strings. Generated forests and reports remain outside this repository.

The model-token inventory is also a caller-hash-pinned `en-AU` JSONL resource.
It must contain exactly the 17 required controls and one mapping for every
segment in the pinned inventory, with strictly increasing unique 16-bit model
IDs and an explicit model input budget. The native serializer emits atomic
`BOS/WB/SYL/STRESS/segment/END/EOS` sequences, never emits `PAD`, chooses
paragraph/sentence/clause/comma boundaries in that order when a chunk is
required, and marks both sides of a forced word-boundary continuation. No
product inventory or final token IDs have yet been frozen.

The synthesis job path now invokes the resolved-frontend operation that joins
the core resources and optional reviewed overlays. It requires one admission
class and segment inventory, binds the LTS model to the exact loaded lexicon and the token
inventory to a caller-pinned frontend ABI, and requires product lexicon/LTS
review hashes to agree. It resolves
role-qualified lexicon entries before LTS, retains per-word provenance, and
feeds the result directly into bounded token packing with no partial output on
failure. A known grapheme with unresolved role variants fails as ambiguous
instead of falling through to LTS. An optional explicitly loaded `local-user`
dictionary now precedes the product/base lexicon without weakening the admitted
base/LTS chain; its exact resource hash remains in the result. Typed request
overrides precede that dictionary: exact phone-syllable records must target one
spoken word and use the installed segment inventory, while replacement text
re-enters lexical scanning once. Replacement words, phrases, diagnostics, and
failures are mapped back to the original request byte span, and override
boundaries may not split a Unicode grapheme. Overlap, malformed replacement
text, an unknown segment, or an ambiguous phone target fails with the indexed
override and no partial output. The installed fixture resources are synthetic;
no fixture resource can claim product admission.

An optional strict contextual-heteronym JSONL resource can now assign a role
when the caller did not provide one and no exact phone override owns the word.
The resource is hash-bound to the exact base lexicon and, in product admission,
to the same review record. Rules inspect only canonical target words, exact
capitalization and clause position, and sorted literal word predicates within a
three-word window on either side, with at most 64 rules for one target. Every
rule role and documented default must exist in the bound lexicon. Exactly one
match selects the role; no match emits `HETERONYM_DEFAULTED`, while overlap
emits `HETERONYM_RULE_AMBIGUOUS` and then uses the documented default. Rule
hash, role source, and rule ID remain in the internal result. Current rules and
tests are first-party synthetic fixtures; there is no product-admitted
Australian rule table yet.

An optional productive-morphology resource now sits after whole-word dictionary
resolution and before LTS. It is bound to the exact base lexicon, segment
inventory, admission class, and product review record. The bounded first-party
mechanism proposes common plural/possessive `-s`, past `-ed`, and progressive
`-ing` stems, but applies a suffix only when exactly one proposed stem resolves
independently through the user or base lexicon. The resolved final segment
selects the plural and past-tense allomorph; progressive `-ing` starts an
unstressed suffix syllable. A whole-word entry always wins, unresolved known
role variants remain errors, and multiple resolvable stems emit
`MORPHOLOGY_AMBIGUOUS` before LTS. Resource hash, stem spelling, inflection kind,
and stem source remain in the internal result. The rule table and tests are
synthetic fixtures; no qualified Australian morphology resource has product
admission yet.

An optional weak-form JSONL resource now performs the first bounded
postlexical selection after all words have pronunciations and before model-token
packing. It binds the exact base lexicon, segment inventory, admission class,
vowel segment class, and product review record. Rules name an existing reviewed
lexicon role and may match capitalization, phrase position, and whether the next
resolved segment is a vowel, a non-vowel, or absent. This makes contexts such as
`the hour` depend on phones rather than the next written character and prevents
lookahead across a phrase boundary. Only an unmodified base/product-lexicon
default is eligible: a typed phone override, explicit role, local dictionary,
morphology, LTS, or spelling result is never rewritten. Exactly one match is
required; overlap emits `WEAK_FORM_RULE_AMBIGUOUS` and retains the strong
default. Resource hash, selected role source, and rule ID remain in the internal
result. Current pronunciations and rules are synthetic fixtures; no
product-admitted Australian weak-form table exists yet.

`tools/build_en_au_lexicon_candidate.py` converts an explicitly hashed CMUdict
snapshot into a deterministic candidate and Australian-review queue outside the
tree. Its output records both source and generator revisions/hashes, is
deliberately labelled unreviewed, and cannot enter a model or installed package
until the General Australian pronunciation gate is signed off.
