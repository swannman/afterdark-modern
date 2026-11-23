# After Dark Modern

Run real **After Dark** 68K and PowerPC screensaver modules under CPU emulation on
modern macOS. Two hosts (`engine/`) execute the original module code against a
software QuickDraw and stream frames; a SwiftUI app and a real macOS screen
saver present them.

**There are no prebuilt downloads** — this is a build-from-source project (one
command, below). If you just want After Dark running on a modern Mac without
building anything, try [Afterglow](https://morphing.cloud/afterglow/), a
separate, unaffiliated project that ships a signed, ready-to-run app and screen
saver for the 68K modules.

Everything in this repo is original work. **No copyrighted After Dark bits are
included** — first run downloads them from archive.org (you're responsible for
sourcing After Dark legally). The CPU emulators come from
[resource_dasm](https://github.com/fuzziqersoftware/resource_dasm), fetched at
build time.

## Layout

- `engine/` — the emulation hosts (`adhost68k.cc`, `adhost.cc`) and the
  software QuickDraw (`ad_toolbox.{cc,hh}`)
- `tools/` — utilities: `adfetch/` downloads/extracts the original assets
  (checksum-verified); the rest are debugging/validation aids
- `app/AfterDark/` — the app, plus `adrender` (headless renderer/verifier)
- `app/saver/` — `AfterDark.saver`, the installable macOS screen saver
- `docs/RESEARCH.md` — the reverse-engineering log (what the hosts expect and why)

## Build & run

You need an Apple Silicon Mac on macOS 14 or later, the Xcode Command Line Tools
(`xcode-select --install`), and three Homebrew packages:

```bash
brew install cmake unar hfsutils
```

Then:

```bash
git clone https://github.com/swannman/afterdark-modern.git
cd afterdark-modern
./build.sh
```

`build.sh` fetches and builds the CPU emulators at pinned revisions, builds the
two hosts, the app and the screen saver (a few minutes the first time), and
prints these two next steps:

```bash
# 1. the app — first launch downloads the original modules (~450 MB from
#    archive.org, checksum-verified), then shows the module picker
cd app/AfterDark && swift run AfterDark

# 2. the screen saver — macOS offers to install it; then pick "After Dark" in
#    System Settings > Screen Saver. Run the app once first: the saver uses the
#    modules the app downloaded.
open app/saver/AfterDark.saver
```

The saver bundle carries its own copy of the hosts and reads the modules from
`~/Library/Application Support/AfterDarkModern`, so it keeps working if you move
or delete the checkout afterwards.

### Details

- **Dependencies are pinned.** The emulators come from a
  [resource_dasm](https://github.com/fuzziqersoftware/resource_dasm) fork plus
  its support library phosg; neither keeps a stable API, so
  `tools/build_deps.sh` checks out the exact pair of revisions the hosts are
  written against. Change the pins there (and nowhere else) to try a newer
  emulator.
- **Individual steps**, if you'd rather not use `build.sh`:
  `tools/build_deps.sh`, then `make -C engine adhost adhost68k`, then
  `swift run` in `app/AfterDark`, then `bash app/saver/build_saver.sh`.
- **A standalone `AfterDark.app`**: `app/AfterDark/make_app.sh` builds a
  self-contained bundle that includes the extraction tools (so it needs no
  Homebrew at run time). Building it requires full Xcode, not just the Command
  Line Tools.
- **Headless**: `tools/adfetch/adfetch.sh` does the same download the app does;
  `swift run adrender --smoke <module-id> 6` drives one module through the full
  pipeline without a window.

## License

Original code under `LICENSE`. `resource_dasm`/`phosg` have their own licenses.
The downloaded After Dark assets remain © their rightsholders.
