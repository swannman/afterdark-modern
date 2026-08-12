# After Dark Modern

Run real **After Dark** 68K and PowerPC screensaver modules under CPU emulation on
modern macOS. Two emulation hosts drive the original module code through a
software QuickDraw implementation and stream rendered frames; a SwiftUI app (and a
headless renderer) present them.

This repository contains **only original work** — the emulation hosts, the
software QuickDraw layer, the fetch tooling, and the app. It ships **no copyrighted
After Dark bits**: the original module resources and the AD 4.0 shared libraries are
downloaded on first run by `tools/adfetch` (see *Assets*, below). You must own /
legally source After Dark to use them.

## What's here

| path | what it is | authorship |
|------|------------|------------|
| `tools/adhost/adhost68k.cc` | 68K host: drives a module's 68K code, emulates the Mac Toolbox traps it calls | ours |
| `tools/adhost/adhost.cc` | PowerPC host (AD 4.0 PEF modules) | ours |
| `tools/adhost/ad_toolbox.{cc,hh}` | software QuickDraw + shared toolbox used by both hosts | ours |
| `tools/adfetch/` | downloads + extracts the original modules and shared libs (resource forks intact), checksum-verified | ours |
| `tools/adblit/` | sprite/RLE decode helper | ours |
| `app/AfterDark/` | SwiftUI app + `adrender` headless renderer + `EmulatedHost` driver | ours |
| `third_party/` (you clone it) | `resource_dasm` (the CPU emulators) + `phosg` | *not included* |

## The CPU emulator (what a rewrite plugs into)

The hosts do **not** implement CPU emulation themselves — they link against
**`resource_dasm`**'s `M68KEmulator` and `PPC32Emulator` (plus `MemoryContext` /
`EmulatorBase`). That is the piece a rewrite replaces. The hosts use, per emulator:

- **`M68KEmulator`** — construct over a `MemoryContext`; set PC/registers; install an
  opcode/PC debug hook (`set_debug_hook`) used to intercept A-line Toolbox traps;
  single-step / run. See `adhost68k.cc` (search `M68KEmulator`, `set_debug_hook`,
  the trap dispatch).
- **`PPC32Emulator`** — same shape for PEF modules; see `adhost.cc`.

### Verifying a rewrite against the corpus

The hosts are deterministic. Frame hashing is the emulator gate:

```bash
# per-frame hashes over N frames; byte-identical output == emulator-equivalent
ADSCREENW=512 ADSCREENH=384 ADFBHASH=1 ADFRAMES=200 \
  tools/adhost/adhost68k "<module>/..namedfork/rsrc" >/dev/null
```

- `ADFBHASH=1` hashes the index plane; `ADFBHASHRGB=1` hashes the decoded RGB
  (needed for CLUT-only animators — the index plane is blind to palette animation).
- A good A/B: run the whole module corpus on the old emulator vs the rewrite and
  diff the hash streams. **Only** genuine emulator-semantics differences should move.
  Note two lessons baked into `docs/RESEARCH.md`: (1) always self-test a census
  harness on a known-*differ* case before trusting an "all identical" result;
  (2) drive each module in its intended screen/lane and, for control-latching
  modules, with its factory control values.

## Build

```bash
git clone <this repo> afterdark-modern && cd afterdark-modern

# 1. dependencies (the CPU emulators + phosg) — clone into third_party/
git clone https://github.com/fuzziqersoftware/phosg          third_party/phosg
git clone https://github.com/fuzziqersoftware/resource_dasm   third_party/resource_dasm
#   (to test a rewrite, point third_party/resource_dasm at your fork/branch)
cd third_party/phosg && cmake -S . -B build -DCMAKE_INSTALL_PREFIX=../local \
  && cmake --build build -j8 && cmake --install build && cd ../..
cd third_party/resource_dasm && cmake -S . -B build -DCMAKE_PREFIX_PATH=../local \
  && cmake --build build --target resource_file m68kdasm resource_dasm -j8 && cd ../..

# 2. the hosts
cd tools/adhost && make && cd ../..

# 3. assets — the ORIGINAL modules + AD 4.0 shared libs (your responsibility to source)
#   The GUI app fetches these automatically on first run (see "Run"); this manual
#   step is the same tool for the headless/host workflow. Either way it downloads
#   from the archive.org sources already set in tools/adfetch/sources.conf (edit to
#   point elsewhere), which are HFS disk images — hence the extraction prerequisites:
brew install unar hfsutils          # + curl, python3 (usually already present)
tools/adfetch/adfetch.sh            # -> places ADShared40.pef + AD4Library.rsrc, extracts modules

# 4. the app (optional; the hosts + adrender are enough to test the emulator)
cd app/AfterDark && swift build
```

`make` in `tools/adhost` compiles `adhost68k` and `adhost` against
`third_party/resource_dasm`. `adfetch` places `ADShared40.pef` + `AD4Library.rsrc`
next to the hosts and extracts the modules; none of those are committed.

## Run

```bash
# headless, straight through a host — one module, 200 frames, hashes to stdout:
ADSCREENW=512 ADSCREENH=384 ADFBHASH=1 ADFRAMES=200 \
  tools/adhost/adhost68k "<AD Deluxe dir>/<Module>/..namedfork/rsrc"

# through the app's EmulatedHost pipeline (resolves the catalog recipe), smoke test:
cd app/AfterDark
.build/release/adrender --smoke 68k-bogglins 20      # -> framesConsumed=N OK

# the GUI app (needs a bundle; SwiftUI won't show a window under bare `swift run`):
./make_app.sh && open AfterDark.app
```

The app's **first run** shows a consent/download gate (`FirstRunManager` →
the bundled `adfetch`) and fetches assets from archive.org to
`~/Library/Application Support/AfterDarkModern/` — no manual `adfetch` needed.
It first checks for the extraction tools (`unar`, `hfsutils`, `curl`, `python3`)
and, if any are missing, shows guidance to `brew install` them (it can't install
them for you). After the fetch it lists the module roster from
`app/AfterDark/Sources/AfterDarkKit/Resources/catalog.json` and runs each module
through `EmulatedHost`.

## Assets & licensing

`tools/adfetch` downloads original After Dark data; **you** are responsible for
sourcing it legally. This repo contains none of it. The hosts and QuickDraw layer
are original work (see `LICENSE`). `resource_dasm` and `phosg` are third-party
(their own licenses).

## Notes

`docs/RESEARCH.md` is the reverse-engineering log — how the Toolbox traps, the AD
3.0/4.0 loaders, and the module quirks were worked out. Useful when a rewrite makes
a module render differently and you need to know what the host expects.
