# Native macOS build (no Docker)

Running strfry under Docker Desktop for Mac routes the LMDB database through the VM
file-sharing layer (virtiofs) and caps the OS page cache at the VM's RAM allocation.
For a large database this makes queries — especially cold scans that miss the cache —
dramatically slower. Building strfry **natively** mmaps the DB directly and lets it use
all of the host's RAM as page cache.

Benchmark (same 50k-event dataset, this machine):

| operation                | native + APFS | native + ExFAT | Docker + ExFAT |
|--------------------------|---------------|----------------|----------------|
| import 50k (write)       | 2.75s         | 2.74s          | ~3.0s          |
| scan 30k kind:1 (read)   | **0.05s**     | **0.05s**      | ~0.36s (+0.42s container start) |

Writes are CPU-bound (signature verification) so the backend barely matters. Reads are
~7x faster native even on a tiny, fully-cached DB; the gap grows much larger on a big DB
that exceeds the Docker VM's RAM, where Docker is forced into cold virtiofs reads.

## Prerequisites

```sh
brew install flatbuffers lmdb libuv openssl@3 zstd secp256k1
```

The Perl codegen modules (`Regexp::Grammars`, `Template`) are vendored under
`golpe/vendor/`, so no CPAN installs are needed.

## Build

```sh
./build-native-macos.sh
```

This:
1. Copies the macOS (kqueue) port of `file_change_monitor.h` from `patches/` into the
   `golpe/external/hoytech-cpp` submodule. The upstream file is Linux-only (inotify);
   the port is `#ifdef`-guarded so Linux builds are unchanged.
2. Exports `CPATH`/`LIBRARY_PATH` so every compile step (including uWebSockets and the
   golpe code generator) finds the keg-only Homebrew headers/libs.
3. Runs `make`.

## Run

```sh
DYLD_LIBRARY_PATH="$(brew --prefix)/lib" ./strfry --config <your.conf> relay
```

Put the database on a real filesystem (APFS), **not** ExFAT — ExFAT has no journaling
and weak locking, which is both slow and risky for LMDB.

## Portability changes

- `golpe/external/hoytech-cpp/hoytech/file_change_monitor.h` — kqueue implementation for
  macOS/BSD (instant notification like inotify, so strfry's real-time delivery is
  preserved). Shipped as `patches/file_change_monitor.h` because the file lives in a
  nested submodule.
- `src/Bytes32.h` — cast `&buf` to `uint64_t*` (on macOS `size_t` != `uint64_t`).
- `src/PluginEventSifter.h` — `st_mtim` → `st_mtimespec`, `environ` via `crt_externs`.
- `Makefile` — link `-luv` on macOS (uWebSockets uses libuv instead of epoll).
