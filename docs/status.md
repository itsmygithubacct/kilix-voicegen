# Current status

Where development stands: what the engine can do today and which gates remain open. Moved from the README.

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
