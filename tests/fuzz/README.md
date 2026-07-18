# Fuzz tests

Fuzz targets, dictionaries, and bounded seed corpora live here. Generated
corpora and crash artifacts are build outputs and are never tracked.

The Stage 15 gate runs ten independent ASan libFuzzer surfaces for at least one
million mutation iterations each via `scripts/ci/run-fuzz.ps1`. Tracked seeds
are copied into ignored per-target corpora; minimized/generated units stay in
the build tree.
