# Third-party boundary

The build consumes utf8proc from a clean external checkout pinned at revision
`0075ed7d0adba45682ee6bf7a83b10f8fd110163` (utf8proc 2.11.3, Unicode 17.0.0).
It provides strict Unicode properties and NFC composition for the product-owned
frontend. utf8proc is unmodified and is licensed under MIT/Expat terms; its
generated Unicode data carries the Unicode data notice. The authoritative
license and complete history remain in the external upstream checkout rather
than this repository. Installed packages copy the license text to
`licenses/utf8proc-LICENSE.md`.

No third-party model, voice, lexicon, recording, or generated audio is included
in this repository.

The future native runtime closure is pinned in
`cmake/dependencies.lock.json`. A pin is not a license grant. Adapted files must
be entered in this ledger with their origin, exact revision, license, local
modifications, and notice disposition before they are committed.

Current implementation files are original project code. The JSON parser and
SHA-256 implementation use published format/algorithm specifications and do
not copy an upstream implementation.
