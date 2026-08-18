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

`make` (see the Makefile header for the third_party dependency build). Plain
resource_dasm master works: the 2026 M68KEmulator rewrite plus the fix PRs
(#102, #103, both merged) is validated against the full module corpus. The
68K host probes the emulator at startup and warns, naming any missing fix,
then runs anyway; the PowerPC host is correct on the same revisions.

## Running

Both hosts take a module file (the resource fork via `..namedfork/rsrc` for 68K)
and stream frames over the protocol the app's `EmulatedHost` speaks. Behavior is
controlled by environment variables; see the KEYENV table in each host's source
for the reference. Useful for standalone work: `ADSCREENW`/`ADSCREENH`,
`ADFRAMES`, `ADFBHASH=1`/`ADFBHASHRGB=1` (per-frame hash streams — the hosts are
deterministic, so hash equality means emulator-equivalent output).
