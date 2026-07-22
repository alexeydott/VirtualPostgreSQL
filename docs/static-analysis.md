[← Previous: Metadata and cache](metadata-functions-cache.md) · [Back to README](../README.md) · [Next: Sanitizers →](sanitizers.md)

# Static analysis

Currently, compiler warnings are errors and three independent Windows contours run:

- MSVC `/analyze` for Win32 and x64;
- selected `clang-tidy` correctness checks with all selected diagnostics promoted to errors;
- PVS-Studio General Analysis for CMake-generated Visual Studio Win32 and x64 solutions.

Run the complete gate with:

```powershell
pwsh -NoProfile -File scripts/ci/run-static-analysis.ps1
```

`PVS-Studio_Cmd.exe` is supplied by the developer or CI environment and defaults to
`D:\tools\PVS-Studio\PVS-Studio_Cmd.exe`. The executable, license state, dependency cache and
`.plog` reports are build inputs/outputs and are not committed or packaged.

PVS-Studio level 1 diagnostics are blocking. Level 2 diagnostics are counted and reviewed on every
run; they remain advisory when they concern ABI layout, deliberate concurrency rechecks,
idempotency tests, or analyzer limitations. Level 3 is informational. No diagnostic-code-wide
source suppression is applied, and a new level 1 diagnostic is never accepted as baseline.

MSVC warnings C6385 and C6387 are disabled only for the `vps_metadata` target because the analyzer does not
model the checked `vps_memory_allocate` byte-size contract. The rowset implementation retains
explicit cell-index, byte-offset, allocation-result and copy-length guards; all other `/analyze`
diagnostics remain warnings-as-errors.

MSVC `/analyze` is attached to production targets. Test executables still compile under `/W4 /WX`
and are included in both PVS-Studio architecture scans; this avoids treating deliberately large,
short-lived test fixtures as production stack allocations while preserving independent test-code analysis.

## See Also

- [Building](building.md) — complete Windows gate commands.
- [Sanitizers](sanitizers.md) — runtime memory/UB analysis.
- [Platform support](platform-support.md) — tested architectures.
