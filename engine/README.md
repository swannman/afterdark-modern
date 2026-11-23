# engine — After Dark runtime hosts

Two hosts run the **real** After Dark module code under CPU emulation
(resource_dasm's emulators over `MemoryContext`, with a debug hook intercepting
Toolbox traps):

- `adhost` — PowerPC A4gm modules. Links the module PEF (data fork) with the
  real `After Dark 4.0 Shared` engine (`ADShared40.pef` = adxpl510) in one
  emulated address space, calls the module's `main`, and drives it
  frame-by-frame. `AD4Library.rsrc` supplies the engine's companion resources;
  `tools/adfetch` installs both files into this directory.
- `adhost68k` — 68K ADgm modules (After Dark 3.x / Deluxe). Emulates the module
  against a software QuickDraw/Toolbox (`ad_toolbox_px.hh`; `ad_toolbox.hh` is
  compiled only into the QDFONT translation unit).

## Build

`../tools/build_deps.sh` first (it fetches and builds resource_dasm + phosg at
the pinned revisions the hosts are written against), then `make adhost adhost68k`
— or just `./build.sh` from the repo root, which does both.

The pins matter: resource_dasm is a perf fork (upstream plus a MemoryContext
arena-refcount fix and the PPC decode cache / basic-block engine), and neither
it nor phosg keeps a stable API, so an unpinned clone of either stops building
against this code. To move to a newer emulator, change the two revisions at the
top of `tools/build_deps.sh` and re-validate the module corpus.

## Running

Both hosts take a module file (the resource fork via `..namedfork/rsrc` for 68K)
and stream frames over the protocol the app's `EmulatedHost` speaks. Behavior is
controlled by environment variables; see the KEYENV table in each host's source
for the reference. Useful for standalone work: `ADSCREENW`/`ADSCREENH`,
`ADFRAMES`, `ADFBHASH=1`/`ADFBHASHRGB=1` (per-frame hash streams — the hosts are
deterministic, so hash equality means emulator-equivalent output).
