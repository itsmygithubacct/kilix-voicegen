# Kilix Voicegen

Kilix Voicegen is a private, offline English text-to-speech engine under active
development. Its v1 product target is one original female-presenting voice and
one original male-presenting voice, both speaking General Australian English,
at or above the selected Piper quality and CPU-operability floor.

## Current status

Phase 2 milestone P0 (foundation and frozen contracts) is complete. The
repository provides the frozen C ABI draft, strict model-package verification,
strict UTF-8/control validation, a typed lexical frontend trace, a native CLI,
and a deterministic fixture engine. P1 implementation is active: the pinned
utf8proc/Unicode 17 dependency now supplies NFC with original-byte spans, and the
staged lexical/normalization vectors execute. Reviewed pronunciation data, a
product model-token inventory, recording pilots, and neural inference remain
open. A strict pronunciation-resource ABI and native loader now bind canonical
`en-AU` JSONL
to caller-pinned file and segment-inventory hashes, enforce bounded NFC entries
and role-qualified syllables, and require a separate review-record hash before a
resource can be admitted as product data. No product lexicon has yet passed that
human review gate. A compact deterministic LTS decision-forest trainer and
native inference loader now provide the corresponding offline fallback
mechanism, but only first-party in-memory/test data have exercised it; no
product LTS artifact exists. The fixture emits a quiet triangle-wave test signal
so streaming, backpressure, cancellation, corruption handling, and language
bindings can be tested without a model.

The v1 frontend and model contract is fixed to `en-AU`; it has no automatic
dialect chooser. British English, then General American English, are
whole-release recruitment/licensing fallbacks that require a new contract.

**This snapshot does not yet produce speech.** It contains no trained model,
recorded voice, downloaded checkpoint, or generated audio.

## Build and test

An out-of-tree CMake build needs a C++17 compiler, Python 3.11 or newer, and a
clean checkout of utf8proc at the exact revision in
`cmake/dependencies.lock.json`. Point CMake at that source checkout; it is built
statically and the installed runtime has no network or external-library lookup.

```sh
export KGV_UTF8PROC_SOURCE_DIR=/path/to/utf8proc
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

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
WAV output. With the current fixture package, that output is a test tone, never
speech. Generated audio and model packages must stay outside this repository.

## Runtime contract

- strict UTF-8 input, explicit `prose` or `terminal` profile, and a 64 KiB hard
  request ceiling;
- one implicit General Australian English (`en-AU`) frontend shared by both
  voices;
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

An internal resolved-frontend operation now joins the three resource types. It
requires one admission class and segment inventory, binds the LTS model to the
exact loaded lexicon and the token inventory to a caller-pinned frontend ABI,
and requires product lexicon/LTS review hashes to agree. It resolves
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
override and no partial output. This seam uses synthetic fixtures only and is
not yet called by the public synthesis engine.

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

`tools/build_en_au_lexicon_candidate.py` converts an explicitly hashed CMUdict
snapshot into a deterministic candidate and Australian-review queue outside the
tree. Its output records both source and generator revisions/hashes, is
deliberately labelled unreviewed, and cannot enter a model or installed package
until the General Australian pronunciation gate is signed off.
