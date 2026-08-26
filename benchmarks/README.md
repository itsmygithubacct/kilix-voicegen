# Benchmark input boundary

The Phase 1 corpus, method, provenance, and baseline report are not copied into
the product repository. `baseline.lock.json` identifies the reviewed immutable
inputs by logical name, byte count, record count where applicable, and SHA-256.

`tools/verify_external_inputs.py --root DIR` verifies a caller-supplied copy.
It never downloads inputs and has no default machine path. A changed research
input requires an explicit new lock revision; it is not silently accepted.
