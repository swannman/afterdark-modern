# After Dark Modern

Run real **After Dark** 68K and PowerPC screensaver modules under CPU emulation on
modern macOS. Two hosts (`engine/`) execute the original module code against a
software QuickDraw and stream frames; a SwiftUI app and a real macOS screen
saver present them.

Prebuilt downloads are on the
[Releases page](../../releases) — see `INSTALL.md` in the zip (or
[docs/INSTALL.md](docs/INSTALL.md)). To build from source, read on.

Everything in this repo is original work. **No copyrighted After Dark bits are
included** — first run downloads them from archive.org (you're responsible for
sourcing After Dark legally). The CPU emulators come from
[resource_dasm](https://github.com/fuzziqersoftware/resource_dasm), cloned
separately.

## Layout

- `engine/` — the emulation hosts (`adhost68k.cc`, `adhost.cc`) and the
  software QuickDraw (`ad_toolbox.{cc,hh}`)
- `tools/` — utilities: `adfetch/` downloads/extracts the original assets
  (checksum-verified); the rest are debugging/validation aids
- `app/AfterDark/` — the app, plus `adrender` (headless renderer/verifier)
- `app/saver/` — `AfterDark.saver`, the installable macOS screen saver
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
cd engine && make adhost adhost68k && cd ..

# app — first launch shows the asset-download gate, then the module picker
cd app/AfterDark && swift run AfterDark

# screen saver bundle (install by double-clicking AfterDark.saver)
bash app/saver/build_saver.sh
```

The `.app` bundle is self-contained — `make_app.sh` builds the extraction tools
(via `tools/get_extractors.sh`) and bundles them, so a distributed app needs no
Homebrew. For `swift run` / headless work, either run `tools/get_extractors.sh`
once or `brew install unar hfsutils`; `tools/adfetch/adfetch.sh` does the same
download the app does.

## License

Original code under `LICENSE`. `resource_dasm`/`phosg` have their own licenses.
The downloaded After Dark assets remain © their rightsholders.
