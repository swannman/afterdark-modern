# After Dark on Modern macOS — Reverse-Engineering Reference

What the original After Dark modules are, what they expect from the machine
underneath them, and how the hosts in `engine` supply it.

## Goal and approach

Run the original, unmodified After Dark module code on modern macOS by executing
it under CPU emulation — resource_dasm's `PPC32Emulator` for PowerPC modules and
`M68KEmulator` for 68K modules. Nothing is reimplemented natively: the module's
own code decides what a frame looks like. The hosts supply only the environment
the module was compiled against — the classic Mac Toolbox surface (Memory
Manager, Resource Manager, QuickDraw, timers, Sound Manager), the After Dark
engine libraries loaded as real code, and a framebuffer to draw into.

Authenticity of output is the acceptance bar. Roughly 82 modules ship across the
After Dark collections, many of them procedural (Mandelbrot, Strange Attractors,
Fractal Forest); hand-reimplementing their behavior would be endless and
approximate. Executing the original binaries is both less work and exact.

Two hosts, one architecture:

- `engine/adhost.cc` — PowerPC (`A4gm`) modules on the PPC engine
  `After Dark 4.0 Shared` (`adxpl510`).
- `engine/adhost68k.cc` — 68K (`ADgm`) modules against the classic 68K
  Toolbox and the 68K After Dark engine (`After Dark 3.0 Faceplate` /
  `After Dark 4.0 Library`).

The detailed findings in this document come from the PowerPC side. The 68K host
follows the same architecture — load the real code, trap the OS surface, drive
the module's own entry point, read the resulting canvas — against 68K Toolbox
traps instead of CFM imports.

## Source package

`AD9v11fullpackage.sit` (StuffIt 5) — "After Dark 9 v1.0", a 2003 fan repackage.
Unpack with `unar`, which preserves resource forks. It contains the After Dark
3.0, 4.0 and Deluxe modules plus support files. The "After Dark 9" control panel
is a patched cdev for Mac OS 9.1+ (Apple removed resource compression there);
irrelevant here, since the control panel is never run.

After Dark 4.0 modules live under the assets root (see `tools/adfetch`) at
`extracted/After Dark 9 v1.0 (9:9:03)/After Dark Files/After Dark 4.0/`
— about 22 of them: Art Critic, Bad Dog!, CYb3r W@t, Fish World, Flying
Toasters!, Guernsey Madness, Life & All, Magic Turtle, Marbles!, Messages 4.0,
Out 'n About, Points of View, Psycho Deli, Rainforest, Rock Paper Scissors,
Rodger Dodger, Shadow Agents, Slow Burn, Super Guy, Swirling Magic, Time Flies.
The Deluxe / Ray / Multimodule collections hold the rest of the ~82.

Extracting a module needs no tooling: the PEF *is* the file's data fork, and the
resources are at `<module>/..namedfork/rsrc`.

    cp "<module>" m.module
    cp "<module>/..namedfork/rsrc" m.rsrc

`ADShared.pef` (the engine, from "After Dark 4.0 Shared") and `AD4Library.rsrc`
(from "After Dark 4.0 Library") are shared by every After Dark 4.0 module.

### Tooling

- `unar` — unpack the `.sit` preserving resource forks.
- `resource_dasm` (Fuzziqer) — extracts `snd `→WAV, `clut`, icons; disassembles
  68K `CODE`; `m68kdasm --pef` disassembles PowerPC PEF containers. Its
  `ResourceFile` class backs the hosts' Resource Manager. It has no After Dark
  RLEP decoder — the hosts do not need one, because the module's own engine
  decodes RLEP (see below).
- Dump a resource fork for offline inspection:
  `resource_dasm --data-fork m.rsrc out_dir`.

## Anatomy of an A4gm module

Type `A4gm`, creator `ADrk`. Fat binary: the data fork is a PowerPC PEF
(`Joy!peffpwpc`), the resource fork (~1.9 MB for Flying Toasters) carries
everything else.

| Resource | Meaning |
|----------|---------|
| `CODE 129/130/131` | 68K code segments (e.g. `ToastersD68K`) — readable via m68kdasm |
| `SYMS`/`NAME`/`JUMP`/`DATA`/`CREL`/`DREL` | Preserved linker symbols + relocations; C++ class names survive (`FlyingClient`, `Sprite`, `TrafficCop`, `ObjectFactory`, `OutOfBounds`, registration points, millisecond timing) |
| `cfrg 0` | Code fragment descriptor → the PEF (`pwpc`) in the data fork |
| `ADgm` | A 68K entry stub |
| `dll#` | Sub-library list (`LIB510_admod/ansi/Audio/Canvas/Coord/File/Glue/Memory/module/R…`) |
| `RLEP 22000+` | Sprite frames in After Dark's RLE format (~100 for Flying Toasters) |
| `Rdat 22000+` | 50-byte per-sprite descriptor: dimensions, transparent key, memory sizes |
| `clut 22000` | 234-entry palette (Mac clut, 16 bits/channel) |
| `snd 22000+` | Embedded sounds (18 for Flying Toasters: Fluttering Wings, Toast Eject, Conga Line, Bionic Toaster Loop, …) |
| `PAT# -4604` | Background patterns |
| `OFtb`/`OFst`/`OFmm` | "Compound Frames" / "MMOffsets" / "CalcMemNeeded" — frame-composition and animation metadata (`TMPL`s exist for these three) |
| `THUM 128` | Preview thumbnail |
| `TEXT 1000`, `STR#` | Credits, error strings |

`Rdat` supplies each sprite's width/height and transparent key (Flying Toasters
22000: 79×81, key 0xf801; 22004: 85×98, key 0xf998).

## The two engine families

After Dark Deluxe 4.1 ships ~82 modules in two families, all with creator `ADrk`:

| Family | Count | Code location | CPU | Shared engine | Emulator |
|--------|-------|---------------|-----|---------------|----------|
| `A4gm` | 22 | data fork = PPC PEF (`Joy!peffpwpc`) | PowerPC | `After Dark 4.0 Shared` (shlb/PEF, library name `adxpl510`) | `PPC32Emulator` |
| `ADgm` | 61 | resource fork (68K `CODE`); data fork empty | 68K | `After Dark 3.0 Faceplate` / `After Dark 4.0 Library` (SHDL, 68K) | `M68KEmulator` |

Also present in the package: `AOgm`×5 (After Dark Online), `FACE`×7 faceplates
(creator `ADr3`), QuickTime INITs and Sound Manager support.

## The A4gm module ABI

A module PEF exports exactly one symbol: **`main`**, a TVector in the data
section (Flying Toasters: `1:0x079C`). It is a factory/dispatcher built on the
shared engine. All its imports (165 for Flying Toasters) come from `adxpl510` —
the base classes and all heavy lifting live in `After Dark 4.0 Shared`. The C++
mangled import names give the class model:

- `PortableModule` (base) ← `AfterDarkModule` ← each module's own class.
- Sprite system: `Background`, `CompoundSprite`, `Sprite`, `ArtSprite`,
  `CompoundSequence`, `RLESequence`, `a64KRLESequence` (16-bit), driven by
  `XSpriteSystem`.
- Primitives: `XCanvas` (draw target, owns the plot vectors), `XPalette`,
  `XRGBColor`, `XR` (rect), `XPt`, `XHeap`, `XTimer` (`GetMilliseconds`),
  `XNoiseMaker` (sound), `XHandle`, `XArtDatabase`, `XSoundDatabase`.
- Lifecycle virtuals, exported by `adxpl510` and overridable by the module:
  `DoBlankScreen`, `DoDrawFrame`, `DoInitialize`, `DoLButtonDown/Up/Held`,
  `DoMouseMove`, `DoEnableAnimation`, `DoDisableAnimation`,
  `CreateTheScreenCanvas`, `PerformBlank(Rect*)`.
- Event pump: `GetModuleEvent`, `ModuleEventAvail`, `ModuleEventsOn/Off`,
  `FlushModuleEvents`. vtables are exported (`__vt__14PortableModule`, …).

`adxpl510` itself imports only leaf OS libraries: **InterfaceLib ×252, MathLib
×27, QuickTimeLib ×18** (~297 total, matching its stub count). InterfaceLib is
the classic Toolbox. The engine does its own per-pixel blitting, so QuickDraw is
needed for setup, region/clip bookkeeping and buffer-to-buffer copies — not for
per-pixel work.

### Call convention and selector table

`main(r3 = arg0ptr, r4 = arg1, r5 = short selector, r6 = params)`.

`main` publishes its arguments into engine globals — notably
`theirParams = r6` — alongside `theFrame`, `theirResult` and friends, which are
DATA imports resolved to real `adxpl510` export addresses. A guard
(`setjmp` at Flying Toasters `0x12A08`) returns 0 on the first call, which runs
the one-time init at `0x121D4`; later calls see 1 and branch straight to the
per-message body at `0x125CC`. Selectors dispatch through a jump table at
module TOC + 10232 (0x27F8) with 8 entries:

| Selector | Role |
|----------|------|
| 0 | INIT — `setjmp`, `operator new(88)` + ctor, build the module object into engine global `[r31]`, load resources, decode sprites, capture the screen background, set up. ~6.7k host OS calls for Flying Toasters. |
| 1 | vtable+0x08 — full redraw (~1020 calls) |
| 2 | vtable+0x0C — light fill/draw pass (~86 calls) |
| 3 | vtable+0x10 — **ANIMATE**: the real per-frame render (spawn, animate, composite, present) |
| 4/5/6 | weak no-op `blr` stubs |
| 7 | event dispatch: switches on `event[+0x36]` to vtable+0x14/0x18/0x1C/0x20/0x34 (mouse/button/key) |

Only selector 3 renders animation. INIT is not the whole run: it constructs and
returns, and the module then stays alive to be pumped.

### Exception model

After Dark's `XException`/`theException` unwinds via setjmp/longjmp, not
compiler exceptions. `0x12A08` is `setjmp` (saves LR/CR/SP/TOC, `stmw r13`, and
f14–f31 via `stfd`); `0x12A7C` is `longjmp`. Running the very first instruction
of a module therefore requires a complete PPC FP unit plus `mfcr`/`mtcrf`/
`mffs`/`mtfsf`.

## The init handshake

Before it dispatches any selector, `main`'s one-time init looks up tagged data
`'ADdl'` in the parameter block it was handed. The engine's tagged-data lookup
(`adxpl510` `0x10EC0`) requires bit 17 of `[block+0x0E]` and a table at
`[block+0x28]` of `{fourcc, value}` pairs. The returned blob's first `short`
must be **≥ 768**; otherwise the module sets error 1284 and `longjmp`s straight
back out.

`'ADdl'` is not a module resource — the faceplate synthesizes it and hands it to
the module through the frame. The host therefore passes a synthesized
frame/param block as **arg3 (r6)** carrying a tag table
(`{u16 count; {fourcc, ptr}…}`) with an `'ADdl'` entry whose blob begins with a
version `short` ≥ 768. With that in place the gate clears and INIT runs real
engine code: `SetResLoad`, `Count1Resources`, `Gestalt`, then an environment
check. `Gestalt('aYmm')` must report version 768 so the engine's
`AfterDarkExists`-style check passes.

The frame/param block also carries the control-panel state (below), so the same
structure that unlocks the gate is what makes the module behave like a
configured screensaver.

## The resource database

A module cannot initialize from its own resource fork alone.
`IXArtDatabase` reads `STR# 1000` and opens
**`:Module Resources:After Dark 4.0 Library`** as a second resource file
(`STR# 5` carries the matching "Could not find Resource Database file" error).
The host Resource Manager therefore chains two open files — the module fork
(`ADRSRC`) and the library fork (`ADLIB`) — with real refnums, and honors
`UseResFile`/`CurResFile` so lookups fall through to the right file. Flying
Toasters loads 185 resources from the module and 118 from the library during
INIT.

Implemented Resource Manager surface: `Get1Resource`, `GetResource`,
`Get1IndResource`, `Count1Resources`, `GetIndString`, `GetResInfo`,
`SizeResource`, `CurResFile`, `SetResLoad`, backed by resource_dasm's
`ResourceFile`.

**External sound files must fail cleanly.** `XNoiseMaker::CalcMemNeeded` sizes
embedded `snd ` resources in its first loop and *file-backed* sounds in its
second: `GetResource('mUsk', id)` → `FSMakeFSSpec` → `FSpOpenDF`/`FSpOpenRF` →
`GetEOF`. Stubs that return `noErr` hand the module uninitialized EOF values and
it computes a multi-gigabyte heap. `FSMakeFSSpec`/`FSpOpenDF`/`FSpOpenRF`/
`GetEOF` must return **`fnfErr` (-43)**, whereupon the module skips absent
external sounds and sizes its heap correctly (~0x237782 for Flying Toasters).

## Screen canvas and QuickDraw

`MacScreenPixels`/`MacScreenCanvas` read the GrafPort's depth and bounds
(`GetDepth`/`GetBounds`) and QuickDraw draws into it. A stubbed `GetPort`
returning 0 yields a degenerate canvas and canvas error 513 (`STR# 2`,
"CanvasErrors"). The host must provide:

- a valid CGrafPort + PixMap whose `baseAddr` points at the host framebuffer,
  with real `rowBytes`, bounds (512×384 in the working configuration),
  `pixelSize` 8 and a `clut`, plus a GDevice — returned from `GetPort`,
  `GetMainDevice` and `GetGWorldPixMap`;
- a real `NewGWorld` (offscreen CGrafPort + PixMap + buffer), with
  `GetGWorldPixMap`, `LockPixels`, `GetPixBaseAddr`;
- `CopyBits` (8-bit; resolves `&CGrafPort.portBits` via the 0xC000
  `portVersion`, then does a src/dst-rect pixel copy);
- `FillRect`, `SetOrigin`, `GetClip`/`ClipRect`/`SetClip`, `ForeColor`, and the
  region ops (`NewRgn`/`DisposeRgn`/`RectRgn`/…).

**Monitor geometry is the load-bearing detail.** The engine sizes every
offscreen GWorld from `GetMonitorInfo__PortableModule`, which reads the emulated
GrayRgn/GDevice monitor table. Left degenerate, it returns a 0×0 rect and every
GWorld comes out 1×1. The host traps the function's convergence point (module
region pc `0x1000E2E4`, where r29 holds the output `XR`) and overwrites the `XR`
with the true framebuffer extent. **`XR` layout is `{left, top, right, bottom}`**
(confirmed from the store sequence at `0xE1B8`). Forcing the extent earlier — at
`IMacScreenPixels` entry — corrupts the coordinates its own `LocalToGlobal`
calls then transform, so the override belongs at the convergence point only
(`ADNOSCREENFIX` disables it).

`CopyBits` must clamp both source and destination coordinates against each
pixmap's real extent; clamping only the lower bounds lets the first full-size
composite run past the source buffer and fault.

## The render pipeline

Per animation frame, driven by selector 3, the module:

1. erases the previous frame's dirty rects — iterate sprites, get bounds via
   Background vtable+0x60, erase each old rect through
   `FillRectangle__9MacCanvas` (vtable+0x74);
2. runs `MixToSaveBackCanvas__10Background` (engine `0x1001A970`, Background
   vtable slot +0x78) — a pure canvas→canvas copy gated on `[Bg+0x16]`
   (save-back destination) and `[Bg+0x12]` (mix source), reading no sprite
   fields;
3. resets and steps the sprite systems, advances its own frame counter, spawns
   and moves sprites, blits their current frames;
4. presents with a `CopyBits` from the flying-area/mix GWorld to the screen
   GWorld — which, wired to the host PixMap baseAddr, lands in the host
   framebuffer.

The module's `DrawFrame` virtual opens with three glue calls that are monitor
blanking boilerplate, not drawing: `TOC[0x14]` = `DoBlankScreen` (import 13),
`TOC[0xB0]` = `GetDeepestMonitor` (105), `TOC[0xB4]` = `BlankOtherMonitors`
(110).

### What makes the module actually animate

Three environmental inputs gate all sprite activity. With any of them missing
the module runs cleanly, composites, presents — and emits a black frame.

- **Control values.** Every module reads its control-panel sliders via
  `GetControlValue(i)`, which indexes a 16-bit array reached through
  `*(adxplTOC + 0x3A8)` — i.e. `theFrame`. With no faceplate these are zero and
  the spawn math produces nothing. The host seeds them after INIT (default 50;
  `ADCTRL="v0,v1,…"` to override). For Flying Toasters controls 0..3 are
  density/fps/kind/position.
- **A millisecond clock.** `XTimer::GetMilliseconds` (engine `0x1002D024`)
  bottoms out in `Microseconds` (import 247). `ADUSPERCALL` sets the per-call
  microsecond step.
- **A tick clock.** The frame-step gate compares elapsed ticks against the art
  object's delay `[art+0x68]`; `TickCount` must advance fast enough for it to
  fire (`ADTICKSTEP`).

With controls seeded and both clocks advancing, the module self-drives: Flying
Toasters spawns toasters over time (blank until ~frame 15), flies them
diagonally "out of the sun" with wings flapping and toast drifting, and shows
the "Flying out of the sun" intro text — no host poking of sprite state at all.
Bad Dog! runs identically on the same host and run loop, animating its full
Mac-desktop scene (dog, folders, Clipboard, trash).

Note that a module owns its own animation policy. Flying Toasters drives its art
through `m_pToastersArt` and blits explicit frames via
`DrawFrame__16CompoundSequence` / `DrawFrame__15a64KRLESequence`; it does *not*
use the engine's `CompoundSprite` sequence state machine. Its
`TrafficControl`-style spawn worker does spawn placement and floating-point
trajectory from the slider values. Other modules may pick a different animate
selector — probe by watching OS-call and framebuffer-change deltas per selector.

### Engine sprite structures

Useful when inspecting or driving the engine directly. `Background` fields are
**unaligned** 32-bit:

| Field | Meaning |
|-------|---------|
| `[Bg+0x12]` | mix canvas (composite destination) |
| `[Bg+0x16]` | save-back canvas |
| `[Bg+0x34]` | root of the sprite linked list |
| `[Bg+0x38]` | allocator object (vtable+0x14 allocates) |
| `[Bg+0x3C]` | flat draw array, built from the list |
| `[Bg+0x40]` | sprite count (u16) |
| `[Bg+0x42]` | dirty flag, set by `AddSprite` |

`CompoundSprite` (vtable `0x10045198`, 248 bytes):

| Field | Meaning |
|-------|---------|
| `+0x18/+0x1A/+0x1C/+0x1E` | bounds (top, left, bottom, right) |
| `+0x30` | visible |
| `+0x32` | must be 0 to draw |
| `+0x36`, `+0x38` | dirty flags |
| `+0x40` | frame source (the `CompoundSequence`/`a64KRLESequence`) |
| `+0x44` | current frame — the draw gate |
| `+0x4A`, `+0x4C` | XPt anchor (x, y as int16) |
| `+0x4E` | sub-sequence index |
| `+0x50/+0x54/+0x56/+0x5A` | sequence-stepping state |

The engine's own Background composite chain, all taking `r3 = Background`:

    GetSpriteArray  0x1001B540   walk the linked list into [Bg+0x3C]
    FillMixBuffer   0x1001B774
    Validate        0x1001B8C0   per-sprite draw, gated on [+0x30]!=0,
                                 [+0x32]==0, ([+0x38] || [+0x36])
    MixToSaveBackCanvas 0x1001A970

Only `Validate` consumes `[Bg+0x3C]`, and `GetSpriteArray` is what builds it —
`[Bg+0x3C]` is null until it runs. `Validate` reads the count from `[Bg+0x40]`
and indexes `[[Bg+0x3C] + i*4]`.

Frame validity: `IsValidFrame` requires `0 < frame <= seq[+0xBE]`, so a sprite
with `+0x44 == 0` is always skipped;
`UpdateSpriteMovement__CompoundSprite` branches on `+0x44` — nonzero calls
vtable+0x160 `UpdateSpriteGround` then vtable+0x74
`MoveSpriteAndFrame__9ArtSprite`, zero calls vtable+0x34 `HideSprite`. Frame
numbers come from `GetSequenceFirstFrame` (vtable+0x11C, `0x10020A5C`) applied
to the sub-sequence index.

Poking `+0x44` directly is not sufficient to activate a pooled sprite: the blit
descriptor and X-shift map are set up by `MoveIntoNextSequence` (`0x100203A8`),
and skipping it leaves `a64KRLESequence::DrawFrame` reading a null frame source
(a `BlockMove` thunk then faults). The consistent host-side activation sequence,
useful as a validation harness, is: probe `GetSequenceFirstFrame(sprite, n)` for
n = 1, 2, 3… until nonzero; poke the XPt anchor; call
`MoveIntoNextSequence(sprite, n)`; set `[+0x30]=1`, `[+0x32]=0`, `[+0x38]=1`
(equivalent to `Show` `0x10022FB8` + `SetVisible` `0x10022FB0`); then run the
composite chain above.

Other verified engine addresses: `MaybeDraw__6Sprite` `0x100229F4`,
`MoveSpriteAndFrame__9ArtSprite` `0x1001A190`,
`CompoundSequence::DrawFrame` `0x1001DA60`, `GetFrameRect__16CompoundSequence`
`0x1001DB78`, `NextSequence__14CompoundSprite` `0x1001EE34`,
`QueueSequence` `0x1001EFD8`, `PlaySequence` `0x1001F244`,
`MoveThroughRelativeFrame` `0x100202CC`, `MoveIntoNextSequence` `0x100203A8`,
`AddSequenceToQueue` `0x10020E24`, `AddSprite__10Background` `0x1001AA0C`,
`CalcBlitCondition__11RLESequence` `0x1002B384`, list `First` `0x100211E4` /
`Next` `0x100211F4`, engine TOC `0x10048000`. Disassembly export-table offsets
are TVector addresses, not code addresses — trust the runtime symbol map.

## The RLEP sprite codec

RLEP resources hold After Dark's own RLE-compressed sprite frames. A 32-byte
header:

```
0x00  'RLID'
0x0c  0x00000020
0x10  0x00000002    (version / plane count)
0x18  transparent key (also in Rdat +0x20)
0x20  'CSTM' chunk + subheader
```

followed by the opcode stream. The `CSTM` sub-chunks within one RLEP
(`sub = 0..0x0a`) are the animation frames, counted by `GetTotalFrames` and
blitted by `DrawFrame(frameIndex, …)`. A single RLEP is larger than one
8-bit image of its `Rdat` dimensions because it packs multiple bit depths and/or
a mask.

The decoder is the fully symbol-exported C++ sprite library inside
`After Dark 4.0 Shared` — class `RLESequence`, plus `a64KRLESequence` for 16-bit.
Blitting is a family of depth-specialized functions in PEF section-0
`0x2300–0x6800`; the 8-bit indexed blitter's dispatch loop sits at section-0
offset `0x2874`:

```
0x2874  lbz    r4,[r23]          ; r23 = source RLE stream pointer
0x2878  rlwinm r0,r4,0,28,31     ; r0 = byte & 0x0F  → opcode (low nibble)
0x287C  cmplwi r0,9              ; opcodes 0..9
0x2880  addi   r23,r23,1         ; advance stream
0x2884  bgt    0x2874            ; masking / skip case
0x2888  subi   r3,r2,17680       ; jump-table base (TOC-relative)
0x288C  rlwinm r0,r0,2,0,29      ; opcode*4
0x2890  lwzx   r3,[r3+r0]        ; table[opcode]
0x2894  mtctr r3 ; 0x2898 bctr   ; dispatch
```

Each control byte packs `opcode = byte & 0x0F` (0..9) and
`operand = byte >> 4`. Decode state:

- `r23` — source stream pointer, advanced by `lbz`/`lbzu`.
- `r25` — pixels remaining in the current row, decremented per pixel. Row
  wrapping is governed by this counter, not by any in-stream row marker.
- `r29` — color-map base; pixel index `i` resolves to `*(r29 + i*4)` as a 32-bit
  `XRGBColor`. The map is a reduced per-sprite palette (≤16 or 256 entries)
  built by `AllocateColorMaps`/`AddColorToMap`/`GetReleventCTabIndex` from the
  module's `clut`.
- `r27`/`r26`/`r24`/`r28` — blit context passed to the pixel writer at `0x3EB70`.

Handlers (each ends `b 0x2874`):

| Address | Behavior |
|---------|----------|
| `0x28A0` | skip N transparent: N = high nibble, or the next byte if the high nibble is 0 (`r25 -= N`) |
| `0x28FC` | draw 1 pixel of color map[high nibble] |
| `0x292C` | draw 2 pixels of color map[high nibble] |
| `0x297C` | draw 4 pixels of color map[high nibble] (unrolled) |
| `0x28C0` | run of color map[high nibble], length = next byte |
| `0x29EC` | variable run: color = map[next byte, a full index], count = high nibble (or next byte if 0) |
| `0x2A48` | sub-dispatch on high nibble 0/1 — unconfirmed |
| `0x289C` | `b 0x2CD8` — end-of-row / flush — unconfirmed |

The remaining depth variants (1/2/4/16/32-bit, mask and no-mask) are the sibling
functions in the same range.

**The hosts do not decode RLEP themselves.** The engine's own blitter, entered at
`rleNewBlit` (engine `0x10000004`, args `r3` = RLE source, `r4` = destination
canvas, `r5` ≈ `r3`, an adjacent mask/position record), does it pixel-perfectly
for every depth and every module. During Flying Toasters' INIT it fires 40 times,
every call targeting one transient scratch canvas descriptor (Flying Toasters:
`0x3DE0AC`, header parsing to rowBytes 0x200, dims 512×384, depth 8, row table
at +0x0A, bounds +0x0E..+0x14, depth +0x22). That buffer reads as all zero after
INIT — it is scratch, not a persistent atlas, and the host never interprets its
contents.

## Host design

`engine/adhost.cc` builds one emulated address space containing the real
engine and the real module and drives the module's own entry point.

1. **Load.** `adxpl510` (`ADShared.pef`) and the module PEF go into one
   `MemoryContext`. Module section addresses are computed by parsing the PEF
   container directly (40-byte container header, `sectionCount` at +0x20 u16be;
   28-byte section headers, `totalSize` at +0x08), skipping zero-size sections
   and page-aligning each non-empty section after the previous — exactly as
   `PEFFile::load_into` does. Nothing about section placement is module-specific.
2. **Link.** The module's imports resolve to `adxpl510`'s exports; `adxpl510`'s
   ~297 InterfaceLib/MathLib/QuickTimeLib imports resolve to host sentinel
   TVectors (`0xF00000xx`) that trap into C++ implementations. A zeroed low
   memory page (0..0x4000, as on a real Mac) lets optional null lookups no-op
   instead of faulting.
3. **Serve.** Toolbox implementations: Memory Manager (`NewPtr`, `NewHandle`,
   handle sizing, `BlockMove`, plus the temp/zone calls — `TempNewHandle` really
   allocates, `TempMaxMem`/`TempFreeMem`/`MaxBlock`/`PurgeSpace`, `GetZone`/
   `SetZone`), the two-file Resource Manager, `Gestalt`, `Microseconds`/
   `TickCount`, `p2cstr`/`c2pstr`, the QuickDraw/GWorld surface above, the File
   Manager stubs that return `fnfErr`, and an RNG. Unimplemented imports log and
   return 0; the working set is grown by watching the call trace, which is also
   how the blitter and the render pipeline were found.
4. **Drive.** Call `main` with the synthesized frame/param block, selector 0, to
   construct and initialize; seed the control values; then pump the animate
   selector once per frame with the clocks advancing, reading the framebuffer
   out after each call.
5. **Call into the engine directly when needed.** `call_code(code, toc, args)`
   invokes any engine C++ method from the host (engine TOC `0x10048000`, taken
   from `adxpl510`'s `__initialize` TVector). This is how the Background
   composite chain and the sprite activation sequence were verified.

Frames go to the SwiftUI/Metal window; one host drives every module in its
family.

### Emulator corrections

resource_dasm's `PPC32Emulator` needed real fixes before After Dark code would
run; all live in the emulator-patches file.

- The entire FP unit was stubbed. Implemented ~40 ops: `lfs`/`lfd`/`stfs`/`stfd`
  (± update), `fmr`/`fneg`/`fabs`/`frsp`/`fctiw(z)`, `fadd`/`fsub`/`fmul`/`fdiv`/
  `fsqrt`/`fmadd`/`fmsub`/`fsel`/`fcmpu`/`fcmpo` and their single-precision
  variants, `mffs`/`mtfsf`. Required just to execute `setjmp`.
- `mfcr`/`mtcrf`, multiply/divide, and the logical ops were missing; `xori`/
  `xoris` and `subfic` were unimplemented (`subfic` = `rD = ~rA + EXTS(SIMM) + 1`
  with `XER[CA]` from the carry-out).
- `lmw` had an endianness bug.
- `stwx`/`stwux` wrote host little-endian (`write<uint32_t>` instead of
  `write<be_uint32_t>`), so a `stwx`/`lwzx` round trip byte-reversed pointers.
- `lhax` and `stbx` wrongly executed the *update* form, writing `EA` back into
  `rA` and corrupting base registers across loops.
- `lwz rX,[rX+off]` was wrongly rejected; the `ra == rd` restriction applies only
  to the update form `lwzu`.
- `MemoryContext::set_symbol_addr` had to tolerate multiple symbols sharing one
  address (C++ aliases, vtables, TVectors).

### Running a module

    ADRSRC=m.rsrc ADLIB=AD4Library.rsrc ADDRAWSEL=3 ADFRAMES=N \
    ADCTRL="100,…" ADUSPERCALL=50000 ADTICKSTEP=6 ADFRAMEDIR=out \
    ./adhost ADShared.pef m.module 9000000

The trailing argument is the instruction/call budget. It must be large: the
default of 2000 truncates INIT (Flying Toasters needs ~6.7k host OS calls) and
returns garbage from a half-built module. Running without `ADRSRC` makes INIT
bail after ~175 calls with error 0x104.

Instrumentation is environment-gated: `ADENGINE` (engine calls by name),
`ADTRACE` (init handshake), `ADFBDUMP` (PPM dumps of the framebuffer and
GWorlds), `ADSCANMEM` (report allocated blocks that are >5% non-zero — locates
live pixel buffers), `ADCBLOG` (CopyBits src/dst), `ADSNAPBLIT` (snapshot every
Nth blit), `ADDUMPADDRS`/`ADDUMPW`/`ADDUMPH` (dump an arbitrary address through
the CLUT), `ADNOSCREENFIX` (disable the monitor-extent override), plus the
sprite-poking knobs (`ADVALIDATE`, `ADACTIVATE`, `ADSEQIDX`, `ADPOKE*`,
`ADFORCEFRAME`) used for host-driven validation. A full INIT takes on the order
of tens of seconds, so prefer dumping raw buffers once and reshaping offline
over sweeping parameters across runs.
