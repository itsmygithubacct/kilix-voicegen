# Runtime contract

The fixed v1 ABI contract and the pronunciation, LTS, model-token, heteronym, morphology and weak-form resources the frontend binds. Moved from the README.

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
