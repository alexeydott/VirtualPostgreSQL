[← Previous: Static analysis](static-analysis.md) · [Back to README](../README.md) · [Next: Troubleshooting →](troubleshooting.md)

# Windows sanitizer contours

The current Windows gate runs isolated x64 clang-cl builds under AddressSanitizer and
UndefinedBehaviorSanitizer. Both contours build production libraries, the loadable extension,
unit tests, the SQLite host tests, integration fixtures, and libFuzzer targets. Network tests may
skip only when their explicit runtime environment is absent.

Run the gate with:

```powershell
pwsh -NoProfile -File scripts/ci/run-sanitizers.ps1
```

UBSan uses the installed `clang_rt.ubsan_standalone-x86_64` runtime and
`-fno-sanitize-recover=undefined`; a report therefore fails the process. clang-cl on Windows does
not provide the complete Unix sanitizer family (MemorySanitizer, ThreadSanitizer, LeakSanitizer,
or a standalone integer sanitizer), so those combinations are not represented as passing gates.
Win32 ASan/UBSan is also not claimed by this release contour. Dependency sources are not granted
blanket `-fno-sanitize` exclusions; generated build trees and sanitizer runtime binaries remain
untracked release inputs.

## See Also

- [Static analysis](static-analysis.md) — compile-time diagnostics.
- [Building](building.md) — running all quality gates.
- [Troubleshooting](troubleshooting.md) — evidence fields for failures.
