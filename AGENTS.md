# glyph

C99 Linux font pkg mgr. Fetches signed HTTPS catalog, verifies SHA-256+RSA-2048, extracts ZIP → user fonts dir. **User-scope only** (no sudo/system). XDG-conforming. v0.1.0 skeleton, filling `src/` against `include/*.h` contracts. Design doc: **`pln-c-repo.md`** (read before architectural changes).

## build/test

meson≥0.56+ninja, gcc|clang, libcurl≥7.68, cjson, miniz. cjson/miniz: system pkg-config → fallback WrapDB; `-Dvendor-deps=true` forces wraps.

```
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/glyph --help
clang --analyze -Xanalyzer -analyzer-output=text src/*.c
```

clean: `rm -rf build && meson setup build`. CI: ubuntu-22.04/24.04 × gcc/clang — stay portable. Deb deps: `libcurl4-openssl-dev libcjson-dev meson ninja-build` (+`libminiz-dev` if wrap unreachable).

## compiler flags (build-breaking)

`warning_level=3`+`werror` + non-defaults:
- **`-Wmissing-prototypes`**: non-`static` fns MUST have prototype in `include/*.h` first. New public fn w/o decl = fail.
- **`-Wstrict-prototypes`**: `f(void)`, not `f()`.
- `-Wall -Wextra -Wpedantic -Werror`: warnings=failures, fix don't silence.

Feature macros set globally (don't redefine): `_POSIX_C_SOURCE=200809L`, `_DEFAULT_SOURCE`, `_XOPEN_SOURCE=700`.

## conventions

- **Ownership**: returned `char*`/arrays = **malloc'd, caller frees**. NULL-term arrays: free each entry then array.
- **Errors**: `int`→ `0` ok / `<0` fail (errno kept). lookups→`NULL`.
- **Exit codes** (`glyph.h` `GLYPH_EXIT_*`): 0 ok,1 err,2 usage,3 net,4 integrity,5 not-found,6 already-inst,7 lock,8 missing-dep. Match exactly in `main.c`.
- Logging: `glyph_log_err/warn/info` (stderr), not printf.

## modules (contracts in `include/*.h`, impl in `src/*.c`; check call sites before changing headers)

| mod | hdr | job |
|---|---|---|
| main | — | getopt_long parse, dispatch, exit codes |
| util | util.h | XDG paths, mkdir -p, flock, str/path helpers, atomic file I/O |
| manifest | manifest.h | parse `catalog.json` (cJSON) → `glyph_catalog_t` |
| download | download.h | libcurl, HTTPS-only, progress, resume |
| verify | verify.h | SHA-256(self)+RSA-2048 PKCS#1v1.5 verify+TOFU keys |
| extract | extract.h | miniz ZIP, include/exclude globs, strip_components |
| db | db.h | `installed.json` CRUD |
| fc_cache | fc_cache.h | spawn fc-cache (fork+execvp) |

## security invariants (never break)

- **HTTPS-only**: `glyph_url_is_allowed()` rejects non-https. Never loosen.
- **Traversal**: `extract.c` rejects `..`/abs paths (`glyph_path_is_safe()`) on every extracted entry.
- **Sig verify**: `catalog.json` untrusted until `.sig2` verifies (TOFU, §4.2). `res/glyph.pem` is the live trust anchor whose SHA-256 fingerprint matches the published release key.
- **Atomic writes**: state files via tmp+rename (`glyph_write_file`).
- **Lock**: mutations hold `<data>/glyph/.lock` flock(LOCK_EX|LOCK_NB). Single-threaded.

## code style

**Efficiency-first.** Write the fastest correct implementation; readability is NOT a goal. Constraints: keep safety invariants (see security section above) — never trade correctness/safety for speed. Optimize: tight loops (SHA-256, RSA, extract/decompress), allocation reduction, branch elimination. Add comments ONLY where non-obvious/confusing; don't narrate obvious code.

## workflow

- After C edits: rebuild + `meson test` before done; warnings=failures.
- New subcommand: follow `cmd_*` in main.c (reset optind, getopt_long, return `GLYPH_EXIT_*`).
- Keep fns `static` unless header-worthy (else need prototype).
- Commits: small, per-module, imperative.
- WrapDB hash fail → fall back system pkgs, don't edit wraps unless asked.

## deferred to v2 (don't add w/o asking)

system-scope, `glyph add <url>`, `glyph.lock`, TUI/color (pln-c-repo.md §12).
