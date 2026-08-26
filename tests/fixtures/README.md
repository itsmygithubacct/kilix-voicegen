# Deterministic fixture graph

`fixture.graph` is an original, non-neural, first-party test fixture. It is
covered by the repository's current rights notice and contains no upstream
model structure, weights, voice data, recording, or generated audio.

- Origin: created for Kilix Voicegen Phase 2 foundation tests.
- Purpose: exercise package hashing, tensor-contract rejection, deterministic
  PCM streaming, callback backpressure, and cancellation without a speech
  model.
- License: first-party test fixture; no third-party material.
- SHA-256: `2a37a107d8e87bda61379f484cd142170466c764e80aa2d8a8503b603b2ff33a`
  (also recorded in `fixtures.lock.json` and embedded by the generator in every
  generated test manifest).

The generator writes the complete model package only into the out-of-tree build
directory. No generated model package or PCM belongs in the source tree.
