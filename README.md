# glyph

A small, dependency-light font manager for Linux written in C. 
`glyph` fetches a signed catalog of fonts, downloads archives over HTTPS, verifies their
integrity and installs them properly

`glyph` operates entirely in **user scope** (no `sudo`, no system files) and
follows the XDG Base Directory Specification.

## Status

This project is under heavy development with the first working version out (v0.1.0).
Please note that there will be breaking changes.

## Build

Requirements: 
- a C compiler
- meson ≥ 0.56
- ninja
- libcurl ≥ 7.68
- plus cJSON
- miniz (via system packages or Meson WrapDB subprojects).

```sh
meson setup build
meson compile -C build
meson test -C build           # optional, runs unit tests if any are present
sudo meson install -C build   # installs /usr/local/bin/glyph by default
```


## CLI

```
glyph 0.1.0 -- font package manager

Usage: glyph <command> [options] [args]

Commands:
  index update              Download and verify the catalog
  index status              Show cached catalog information
  search <query>            Search the catalog by id / name / tag
  list [--catalog]          List installed fonts (or catalog fonts)
  info <id>                 Show details for a font
  install <id>[@rev|==ver]  Install a font from the catalog
  remove <id>               Remove an installed font
  upgrade [--all] [<id>]    Upgrade one font (or all installed fonts)

Common options:
  --no-cache                Skip fontconfig cache refresh
  --debug                   Enable diagnostic tracing (or GLYPH_DEBUG=1)
  -h, --help                Show this help
  -v, --version             Show the glyph version

Environment:
  GLYPH_DEBUG               Set to a non-empty value other than 0 to
                            enable diagnostic tracing on stderr
```

Exit codes: `0` success, `1` generic error, `2` usage error, `3` network error,
`4` integrity error, `5` not found, `6` already installed, `7` lock busy,
`8` missing system dependency.

## XDG paths

| Purpose            | Path                                            |
|--------------------|-------------------------------------------------|
| Installed fonts    | `$XDG_DATA_HOME/fonts/<id>/<version>/`          |
| Installed-state DB | `$XDG_DATA_HOME/glyph/installed.json`           |
| Lock file          | `$XDG_DATA_HOME/glyph/.lock`                    |
| TOFU public keys   | `$XDG_DATA_HOME/glyph/keys/<fingerprint>.pem`   |
| Catalog cache      | `$XDG_CACHE_HOME/glyph/catalog.json`            |
| Catalog signature  | `$XDG_CACHE_HOME/glyph/catalog.json.sig2`       |
| Temp downloads     | `$TMPDIR/glyph-XXXXXX/` (defaults to `/tmp/`)   |

Defaults apply when the corresponding `XDG_*` variable is unset: `~/.local/share`,
`~/.cache`, `~/.config`.

## Dependencies

| Purpose              | Library     | Notes                                  |
|----------------------|-------------|----------------------------------------|
| HTTPS downloads      | libcurl     | ≥ 7.68                                 |
| JSON parsing         | cJSON       | 1.7.18 (system or WrapDB)              |
| ZIP / DEFLATE        | miniz       | 3.0.2 (system or WrapDB)               |
| SHA-256 / RSA-2048   | bundled     | Self-contained (verification only)     |
| CLI argument parsing | getopt_long | libc                                   |
| Font cache refresh   | fc-cache    | External binary invoked via `execvp`   |

## Known limitations

- **User-scope only** — no system fonts, no `sudo` install path.
- **HTTPS-only** — plain `http://` URLs in the catalog are rejected.
- **Linux only** — relies on `flock`, `fc-cache`, and XDG conventions.

## License
MIT — see [LICENSE](LICENSE).
