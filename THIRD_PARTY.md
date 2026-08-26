# Third-party boundary

The build consumes utf8proc from a clean external checkout pinned at revision
`0075ed7d0adba45682ee6bf7a83b10f8fd110163` (utf8proc 2.11.3, Unicode 17.0.0).
It provides strict Unicode properties and NFC composition for the product-owned
frontend. utf8proc is unmodified and is licensed under MIT/Expat terms; its
generated Unicode data carries the Unicode data notice. The authoritative
license and complete history remain in the external upstream checkout rather
than this repository. Installed packages copy the license text to
`licenses/utf8proc-LICENSE.md`.

The optional Linux research build consumes ONNX Runtime 1.29.0 through its C
API. CMake requires a clean external source checkout at revision
`2e2543fbe9fae542f921d47a72d21d5a4ef0b710` for the matching headers, license,
and third-party notices, plus an external CPU shared library named exactly
`libonnxruntime.so.1.29.0` with the SHA-256 recorded in
`cmake/dependencies.lock.json`. That binary came from the recorded CPython 3.12
manylinux wheel installation; the wheel archive itself was not retained, so
this is a hash-pinned research binary, not a claimed source-reproducible release
closure. CMake copies the verified library into an enabled build/install and
installs ONNX Runtime's license and third-party notices. Neither the source nor
binary is committed here.

The external research-package generator imports a separately built Piper/eSpeak
phonemizer only while constructing a test-fixture frontend/projection. Piper and
eSpeak are not linked by, copied into, or discovered by the installed synthesis
runtime.

No third-party model, voice, lexicon, recording, or generated audio is included
in this repository. The external Australian-female Piper package is explicitly
research-only and carries its own recording/model-lineage notice.

The native dependency closure is pinned in `cmake/dependencies.lock.json`. A
pin is not a license grant. Adapted files must
be entered in this ledger with their origin, exact revision, license, local
modifications, and notice disposition before they are committed.

Current implementation files are original project code. The JSON parser and
SHA-256 implementation use published format/algorithm specifications and do
not copy an upstream implementation.
