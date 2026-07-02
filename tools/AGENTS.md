# vxcore Diagnostic Tools

Standalone diagnostic executables built from vxcore. They are **runtime diagnostics, not
ctests**: each needs a real notebook on disk and is intentionally NOT registered with
`add_test`. Tools link `vxcore` and call only the public C ABI.

Tools are gated behind the `VXCORE_BUILD_TOOLS` CMake option (default `OFF`), so a normal
build never compiles them.

## search-validate (`vxsearch-validate`)

Cross-checks vxcore content search against **ripgrep (`rg`)** as ground truth. It opens a
real bundled notebook, runs the search **exactly the way VNote does** (streaming primitive
+ multi-threaded drain pool, per the root [Search Threading Contract](../../../AGENTS.md#search-threading-contract)),
and classifies every discrepancy into one of three buckets.

### Build

```powershell
cmake -S libs/vxcore -B libs/vxcore/build_tools -G "Visual Studio 17 2022" -A x64 `
  -DVXCORE_BUILD_TOOLS=ON
cmake --build libs/vxcore/build_tools --config Debug --target vxsearch-validate
```

### Run

vxcore is built **SHARED**, and the root CMake sets
`CMAKE_RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR}/bin`, so the exe **and** `vxcore.dll`
land together in `build_tools/bin/Debug/`. Prepend that dir to `PATH` so the loader resolves
the DLL:

```powershell
$env:PATH = "$PWD/libs/vxcore/build_tools/bin/Debug;" + $env:PATH
& libs/vxcore/build_tools/bin/Debug/vxsearch-validate.exe "<notebook-root>" --pattern "foo"
```

`<notebook-root>` must be a **bundled** notebook (contains `vx_notebook/config.json`).
`rg` must be on `PATH` (the tool aborts otherwise — comparison is impossible without it).

The tool runs in vxcore **test mode** (`vxcore_set_test_mode(1)`) so it never mutates the
user's real `vxcore.json`/session and avoids metadata-DB lock contention with a running
VNote. Because test mode shares `%TEMP%\vxcore_test_config` with vxcore's own test suite, its
session may list many stale notebooks; the tool suppresses the resulting vxcore console log
spam by default. Pass `--verbose` to see vxcore's internal logging.

### Flags

| Flag | Meaning |
|---|---|
| `<notebook-root>` (positional) | Bundled notebook root (required). |
| `--pattern <text>` | Search pattern (required). |
| `--case-sensitive` | Case-sensitive match (default: insensitive). |
| `--regex` | Treat pattern as a regex (**diagnostic-only** mode). |
| `--word` | Whole-word match (**diagnostic-only** mode). Cannot combine with `--regex`. |
| `--folder <subtree>` | Restrict to a notebook subtree (default: whole notebook). |
| `--backend simple\|rg` | vxcore backend to validate (default: `simple`). |
| `--max-results <N>` | File-boundary cap mirroring VNote. `0` = uncapped (default; required for pass/fail — see below). |
| `--limit <N>` | Max detail lines printed per bucket (default: 50). |
| `--strict` | Non-zero exit on ANY non-empty bucket (incl. discovery/diagnostic). |
| `--json` | Emit a machine-readable JSON report to stdout. |
| `--verbose` | Do not suppress vxcore's internal console logging. |
| `-h`, `--help` | Show help. |

`--word --regex` is rejected at parse time: the simple backend **ignores `wholeWord` on the
regex path** (`simple_search_backend.cpp` `RunMatch`), so there is nothing meaningful to
compare.

### The three buckets

vxcore content search only scans files **indexed in the notebook's `vx.json` metadata** (a
metadata tree walk), never a raw filesystem walk. `rg` walks the disk. Let `I` be the indexed
file set, `V` the vxcore match set `(relpath, lineNumber)`, and `R` the `rg` match set. Then:

| Bucket | Definition | Meaning |
|---|---|---|
| **Missed matches** | `R_idx − V` (rg hit an **indexed** file's line that vxcore did not) | **Bug candidate.** |
| **False positives** | `V − R_idx` (vxcore matched a line rg did not, in an indexed file) | **Bug candidate.** |
| **Discovery gaps** | `rg` hits in files **not** in `I` and not infrastructure | **Expected**, not a bug. |

`R_idx` = `rg` hits whose relpath ∈ `I`. Hits under `vx_notebook/` or `.git/` are
**infrastructure** and ignored. Comparison identity is `(relpath, lineNumber)`; column offsets
and per-line match multiplicity are deliberately ignored (they differ by design), and
`lineText` is shown for context only (avoids CRLF/trailing-`\r` noise).

### Why discovery gaps are EXPECTED (not bugs)

`rg` has no notebook-index concept — it matches any file on disk. vxcore only searches
files recorded in `vx.json`. So a discovery gap is any on-disk match `rg` finds that vxcore
never searched because the file is not indexed. Common, benign sources:

- Notes created on disk (or by an external tool) but never imported into the notebook.
- Attachments / assets under the notebook's `vx_assets` folder.
- `vx_notebook/` internal metadata and `.git/` (classified as infrastructure and ignored).

A discovery gap means "this file exists on disk with a match but isn't in the index" — it is
**not** a search bug. It only becomes a warning-worthy signal if you expected the file to be
indexed.

### Validation fidelity policy (per mode)

The tool validates the **current simple-backend semantics** (not desired/product semantics).
`rg` is an authoritative pass/fail oracle only where it can be configured to match those
byte-level semantics exactly; elsewhere its differences are **expected** and are reported but
do **not** fail the exit code (unless `--strict`).

| Query mode | `rg` config | Status |
|---|---|---|
| Literal, case-sensitive | `--fixed-strings -s` | **Pass/fail** — byte substring; both engines agree exactly. |
| Literal, case-insensitive, ASCII line | `--fixed-strings --ignore-case --no-unicode` | **Pass/fail** — ASCII byte-fold matches the backend's `std::tolower`. |
| Literal, case-insensitive, non-ASCII line | same | **Diagnostic-only** — Unicode vs byte case-fold differ; such lines are flagged and excluded from pass/fail. |
| `--word` (literal only) | `--fixed-strings --word-regexp` | **Diagnostic-only** — `rg` treats `_` as a word char while the simple backend treats `_` as a boundary. |
| `--regex` | `-e <pattern>` (no `--fixed-strings`) | **Diagnostic-only** — ECMAScript `std::regex` vs Rust regex dialects differ. |
| `--word` + `--regex` | — | **Rejected** at arg-parse. |

The tool prints a **Fidelity** banner naming the active policy, and routes each discrepancy to
either the pass/fail or diagnostic count. The primary, trustworthy signal is the literal
(case-sensitive, and ASCII case-insensitive) comparison.

**`--max-results` caveat.** `--max-results N` (N > 0) applies VNote's file-boundary cap to the
vxcore side only; `rg` is not capped. When the cap is actually hit, beyond-cap `rg` matches would
otherwise appear as spurious pass/fail "missed" entries, so a truncated run is forced to
**diagnostic-only** (the Fidelity banner and the `truncated` JSON field both say so, and the exit
code stays 0 unless `--strict`). Keep `--max-results 0` (the default) for a real pass/fail check.

### Known benign-discrepancy sources

When diffs appear in diagnostic mode, they are usually one of these — verify before filing a
bug:

- **Regex dialect:** vxcore uses ECMAScript `std::regex`; `rg` uses Rust's regex crate.
  Anchors, classes, and escapes diverge.
- **`_` word-boundary rule:** `rg --word-regexp` treats `_` as a word char; the simple
  backend treats `_` as a boundary (`isalnum` check).
- **Unicode vs ASCII case-fold:** `rg` default case-folding is Unicode-aware; the backend
  folds ASCII bytes only. The tool adds `--no-unicode` to align on ASCII, so only **non-ASCII**
  lines diverge (and those are diagnostic-only).
- **CRLF / trailing `\r`:** identity is `(relpath, line)`, not `lineText`, so EOL noise is
  already excluded.
- **Binary / UTF-16 indexed files:** both the simple backend and `rg --text` read bytes and
  split on `\n`; multi-byte encodings can misalign lines.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Clean — no **pass/fail** missed/false-positive discrepancies (discovery gaps and diagnostic diffs are warnings). |
| `1` | Pass/fail discrepancies found; or, with `--strict`, any non-empty bucket (discovery or diagnostic included). |
| `2` | Usage / preflight error (bad args, not a bundled notebook, `rg` missing, context/open failure). |

### How VNote-faithfulness is preserved

- The search runs on a background `std::thread` calling `vxcore_search_content_streaming`,
  while a drain pool of `N = hardware_concurrency(); if (N==0) N=2; N=min(N,8)` threads (VNote's
  exact rule from `src/core/services/searchservice.cpp`) loops
  `vxcore_work_queue_process_next(ctx, "vxcore.search", 100)`.
- Batches are accumulated thread-safely keyed by `batch_index`, then reassembled in order —
  the same discipline VNote's `SearchWorker` uses. Validating the reassembled streaming result
  against `rg` is explicitly part of the check: it catches concurrency regressions the blocking
  API would hide. (Repeated runs are byte-identical — streaming parity.)
- Backend is pinned per run via `vxcore_context_update_config` (`{"search":{"backends":[...]}}`),
  because `vxcore_context_create` ignores its `config_json` arg and `CreateSearchManager`
  re-reads `GetConfig()` per search.
