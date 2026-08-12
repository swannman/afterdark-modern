# After Dark Modern

Run real **After Dark** 68K and PowerPC screensaver modules under CPU emulation on
modern macOS. Two hosts (`tools/adhost`) execute the original module code against a
software QuickDraw and stream frames; a SwiftUI app presents them.

Everything in this repo is original work. **No copyrighted After Dark bits are
included** — first run downloads them from archive.org (you're responsible for
sourcing After Dark legally). The CPU emulators come from
[resource_dasm](https://github.com/fuzziqersoftware/resource_dasm), cloned
separately.

## Layout

- `tools/adhost/` — the emulation hosts (`adhost68k.cc`, `adhost.cc`) and the
  software QuickDraw (`ad_toolbox.{cc,hh}`)
- `tools/adfetch/` — downloads/extracts the original assets, checksum-verified
- `app/AfterDark/` — the app, plus `adrender` (headless renderer/verifier)
- `docs/RESEARCH.md` — the reverse-engineering log (what the hosts expect and why)

## Build & run

```bash
# dependencies: the CPU emulators (to test a rewrite, use your fork/branch here)
git clone https://github.com/fuzziqersoftware/phosg         third_party/phosg
git clone https://github.com/fuzziqersoftware/resource_dasm third_party/resource_dasm
cd third_party/phosg && cmake -S . -B build -DCMAKE_INSTALL_PREFIX=../local \
  && cmake --build build -j8 && cmake --install build && cd ../..
cd third_party/resource_dasm && cmake -S . -B build -DCMAKE_PREFIX_PATH=../local \
  && cmake --build build --target resource_file m68kdasm resource_dasm -j8 && cd ../..

# hosts
cd tools/adhost && make && cd ../..

# app — first launch shows the asset-download gate, then the module picker
cd app/AfterDark && swift run AfterDark
```

The download needs `brew install unar hfsutils` (the archive.org sources are
StuffIt/HFS images). For headless work without the app, run
`tools/adfetch/adfetch.sh` directly — same tool, same downloads.

## Testing an emulator rewrite

The hosts link `resource_dasm`'s `M68KEmulator` / `PPC32Emulator` (over
`MemoryContext`, with a `set_debug_hook` used to intercept Toolbox traps) — that's
the piece a rewrite replaces. Point `third_party/resource_dasm` at your branch,
rebuild the hosts, and compare frame hashes; the hosts are deterministic, so
byte-identical hash streams mean emulator-equivalent:

```bash
ADSCREENW=512 ADSCREENH=384 ADFBHASH=1 ADFRAMES=200 \
  tools/adhost/adhost68k "<AD Deluxe dir>/<Module>/..namedfork/rsrc"
```

Use `ADFBHASHRGB=1` for palette-animating modules (the index plane can't see CLUT
animation), and sanity-check any "all identical" sweep against a case you know
differs. `adrender --smoke <module-id> <secs>` smoke-tests a module through the
app's full pipeline.

## License

Original code under `LICENSE`. `resource_dasm`/`phosg` have their own licenses.
The downloaded After Dark assets remain © their rightsholders.
