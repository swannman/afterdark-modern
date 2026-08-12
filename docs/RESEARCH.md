# After Dark 4.0 → Modern macOS: Reverse-Engineering Notes

## Goal
Run After Dark 4.0 screensaver modules natively on Apple Silicon macOS, in a
window (screensaver `.saver` integration is a later phase). No emulation. The
original binaries are the source of truth: extract their real art/sound, read
their real behavior from disassembly, reimplement natively (Swift/SpriteKit).

Proof-of-concept module: **Flying Toasters!**

## Source package
`AD9v11fullpackage.sit` (StuffIt 5) — "After Dark 9 v1.0", a 2003 fan repackage.
Extract with `unar`. Contains After Dark 3.0, 4.0, Deluxe modules + support files.
The "After Dark 9" control panel is a patched cdev for Mac OS 9.1+ (Apple removed
resource compression there); irrelevant to us — we don't run the control panel.

## Anatomy of an AD4 module (type `A4gm`, creator `ADrk`)
Fat binary. Data fork = PowerPC PEF (`Joy!peffpwpc`). Resource fork (~1.9 MB for
Flying Toasters) holds:

| Resource | Meaning |
|----------|---------|
| `CODE 129/130/131` | 68K code segments (`ToastersD68K`) — human-readable via m68kdasm |
| `SYMS`/`NAME`/`JUMP`/`DATA`/`CREL`/`DREL` | **Preserved linker symbols + relocations** — C++ class names survive: `FlyingClient`, `Sprite`, `TrafficCop`, `ObjectFactory`, `OutOfBounds`, registration points, millisecond timing |
| `cfrg 0` | Code fragment descriptor → PEF "Flying Toasters DPPC" (pwpc) in data fork |
| `RLEP 22000-22103` | ~100 sprite frames, After Dark RLE format (toaster wing-flap cycle, toast, burnt toast) |
| `Rdat 22000-22103` | 50-byte per-sprite descriptor (dimensions, transparent key, mem sizes) |
| `clut 22000` | 234-entry palette (Mac clut, 16-bit/channel) |
| `snd 22000-22017` | 18 sounds → extracted to WAV (Fluttering Wings, Toast Eject, Conga Line, Bionic Toaster Loop, ...) |
| `PAT# -4604` | Background patterns (starry night sky) |
| `OFtb`/`OFst`/`OFmm` | "Compound Frames" / "MMOffsets" / "CalcMemNeeded" — frame-composition + animation metadata |
| `THUM 128` | Preview thumbnail |
| `TEXT 1000`, `STR#` | Credits, error strings |

## Tooling
- `unar` (Homebrew) — unpack the .sit preserving resource forks.
- `resource_dasm` (Fuzziqer; built from source against local `phosg` in scratchpad)
  — extracts snd→WAV, clut, icons; disassembles 68K CODE; but has **no** After Dark
  RLEP decoder, so we write our own.
- Extract a module's resource fork: `cat "Module/..namedfork/rsrc" > mod.rsrc`
  then `resource_dasm --data-fork mod.rsrc out_dir`.

## RLEP sprite format (IN PROGRESS — the one real RE nut)
32-byte header:
```
0x00  'RLID'
0x0c  0x00000020
0x10  0x00000002   (version? / plane count?)
0x18  transparent key (0xf801 for 22000, 0xf998 for 22004) — also in Rdat 0x20
0x20  'CSTM' chunk + subheader; trailing u16 (0x0c6c/0x0abc) = length? row count?
```
Then an RLE opcode stream. Observations:
- Dominant opcode byte `0x17` (3142x in 22000) → literal-run: `17 NN <NN pixels>`.
- `0x10`, `0x01`, `0x13` recur as group/skip/transparent-run markers (TBD).
- File size (33 KB) ≫ one 79×81 8-bit image (6.4 KB) ⇒ RLEP packs multiple
  bit-depths (1/8/16-bit) and/or mask in one resource.
- Rdat gives width/height (22000: 79×81; 22004: 85×98) and the transparent key.

### CODEC CRACKED — decoder found in `After Dark 4.0 Shared` (PPC PEF)
The sprite engine is a fully symbol-exported C++ library in `After Dark 4.0 Shared`
(disassemble with `m68kdasm --pef`). Key class `RLESequence` (+ `a64KRLESequence`
for 16-bit). The blit/decode is a family of depth-specialized functions; the 8-bit
indexed blitter's dispatch loop is at PEF section-0 offset `0x2874`:

```
0x2874  lbz    r4,[r23]          ; r23 = source RLE stream pointer
0x2878  rlwinm r0,r4,0,28,31     ; r0 = byte & 0x0F      → LOW NIBBLE = opcode
0x287C  cmplwi r0,9              ; opcodes 0..9
0x2880  addi   r23,r23,1         ; advance stream
0x2884  bgt    0x2874            ; (masking / skip case)
0x2888  subi   r3,r2,17680       ; r3 = jump-table base (TOC-relative)
0x288C  rlwinm r0,r0,2,0,29      ; r0 = opcode*4
0x2890  lwzx   r3,[r3+r0]        ; table[opcode]
0x2894  mtctr r3 ; 0x2898 bctr   ; dispatch
```

**Model:** each control byte packs `opcode = byte & 0x0F` (low nibble, 0..9) and
`operand = byte >> 4` (high nibble). Registers during decode:
- `r23` = source stream ptr (advanced via `lbz`/`lbzu`)
- `r25` = **pixels remaining in current row** (decremented per pixel; this — not a
  `0x10` marker — governs row wrapping, which is why `0x10` counts ≠ height)
- `r29` = **color-map base**; a pixel index `i` resolves to color `*(r29 + i*4)`
  (32-bit `XRGBColor`). The map is a reduced per-sprite palette (≤16 or 256 entries)
  built by `AllocateColorMaps`/`AddColorToMap`/`GetReleventCTabIndex` from `clut 22000`.
- `r27`/`r26`/`r24`/`r28` = blit context passed to the pixel-writer at `0x3EB70`.

**Handlers observed (each ends `b 0x2874`):**
| addr | behavior |
|------|----------|
| `0x28A0` | **skip N transparent**: N = high nibble, or next byte if high nibble==0 (`r25 -= N`). This is the `01 <len>` case. |
| `0x28FC` | draw **1** pixel of color = map[high nibble] |
| `0x292C` | draw **2** pixels of color = map[high nibble] |
| `0x297C` | draw **4** pixels of color = map[high nibble] (unrolled) |
| `0x28C0` | run of color map[high nibble], length = next byte |
| `0x29EC` | **variable run**: color = map[**next byte** = full index], count = high nibble (or next byte if 0) |
| `0x2A48` | sub-dispatched (high-nibble 0/1 cases) — TBD |
| `0x289C` | `b 0x2CD8` — end-of-row / flush — TBD |

Other depth variants (1/2/4/16/32-bit, mask/no-mask) are the sibling functions in
`0x2300–0x6800`; the 11 `CSTM` sub-chunks per RLEP (`sub=0..0x0a`) are the animation
frames (`GetTotalFrames`), each blitted by `DrawFrame(frameIndex, ...)`.

### Next step to finish the codec
1. Recover the 10 jump-table targets → opcode↔handler map (read TOC table, or infer
   by decoding + visual check with a placeholder palette to confirm the toaster shape).
2. Transcribe handlers into a standalone decoder (Python) → PNG per frame.
3. Build the color map from `clut 22000` via the `AddColorToMap` logic; validate
   colors against `THUM 128` preview.
Alternative: drive the PEF blitter directly via a PPC emulator to get pixel-perfect
output for every depth/module without hand-transcription.

## Behavior (Flying Toasters) — TODO from disassembly
Toasters (flapping wings, multi-frame cycle) + toast fly diagonally across a starry
night. Recover: spawn edges/positions, per-object speed distribution, flap timing,
toast:toaster ratio, rare/special objects, sound triggers. Symbols to anchor on:
`FlyingClient`, `TrafficCop` (spawn/scheduling?), `Sprite`, `OutOfBounds`.

---

# Phase 2: Full runtime host — run the REAL module code under emulation

Decision (user): "Run the real module code." True 100% fidelity across ~82
heterogeneous modules (many procedural: Mandelbrot, Strange Attractors, Fractal
Forest, ...) is only achievable by executing each module's original code, not by
hand-reimplementing behavior. So we build a minimal **After Dark runtime host** on
top of resource_dasm's CPU emulators and drive the real modules frame-by-frame,
reading their canvas into the native macOS window.

## Two engine families (from After Dark Deluxe 4.1, ~82 modules)
| Family | Count | Code location | CPU | Shared engine | Emulator |
|--------|-------|---------------|-----|---------------|----------|
| `A4gm` | 22 | **data fork** = PPC PEF (`Joy!peffpwpc`) | PowerPC | `After Dark 4.0 Shared` (shlb, PEF, lib name **`adxpl510`**) | `PPC32Emulator` |
| `ADgm` | 61 | **resource fork** (68K CODE); data fork empty | 68K | `After Dark 3.0 Faceplate` / `After Dark 4.0 Library` (SHDL, 68K) | `M68KEmulator` |

Creator for all modules = `ADrk`. Also present: `AOgm`×5 (AD Online), `FACE`×7
faceplates (all creator `ADr3`), QuickTime INITs, Sound Manager.
Start with A4gm (reuses the working PPC + adxpl510 infrastructure); ADgm is a second
host on the 68K emulator later. Rat Race (the maze/solver) is ADgm.

## A4gm module ABI (the important find)
A module PEF exports exactly one symbol: **`main`** (a TVector in the data section,
FT at `1:0x079C`). It is a factory/dispatch built on the shared engine.

The module imports (FT: 165 symbols) come from library **`adxpl510`** — i.e. the
base classes and all heavy lifting live in `After Dark 4.0 Shared`. C++ mangled
names reveal the class model:
- `PortableModule` (base) ← `AfterDarkModule` (base) ← each module's class.
- Sprite system: `Background`, `CompoundSprite`, `Sprite`, `CompoundSequence`,
  `RLESequence`, `a64KRLESequence` (16-bit), driven by `XSpriteSystem`.
- Primitives: `XCanvas` (draw target, has the plot vectors adblit already uses),
  `XPalette`, `XRGBColor`, `XR` (rect), `XPt`, `XHeap`, `XTimer` (`GetMilliseconds`),
  `XNoiseMaker` (sound), `XHandle`.
- Lifecycle/virtuals (all exported by adxpl510, overridable by the module):
  `DoBlankScreen` (**the per-frame draw**), `DoDrawFrame`, `DoInitialize`,
  `DoLButtonDown/Up/Held`, `DoMouseMove`, `DoEnableAnimation`, `DoDisableAnimation`,
  `CreateTheScreenCanvas`, `PerformBlank(Rect*)`.
- Event pump: `GetModuleEvent`, `ModuleEventAvail`, `ModuleEventsOn/Off`,
  `FlushModuleEvents`. vtables exported as `__vt__14PortableModule` etc.

`adxpl510` itself imports only leaf OS libs: **InterfaceLib ×252, MathLib ×27,
QuickTimeLib ×18** (~297 total — matches the stub count). InterfaceLib = classic Mac
Toolbox (Memory Mgr, Resource Mgr, QuickDraw, Sound Mgr, ...). adxpl510 does its OWN
per-pixel blitting (the strip blitter at sec0+0x2838), so QuickDraw is needed only
for setup/screen-copy, not per-pixel.

## Host design (`tools/adhost`)
1. Load `adxpl510` (After Dark 4.0 Shared) + the module PEF into one MemoryContext;
   resolve the module's imports to adxpl510's exports, and adxpl510's imports to host
   sentinel TVectors (0xF00000xx), same trap trick adblit uses for plot vectors.
2. Emulate the ~297 InterfaceLib/MathLib/QuickTime imports as host callbacks in C++:
   Memory Mgr (NewPtr/NewHandle/HLock/...), Resource Mgr (Get1Resource → serve the
   module's RLEP/Rdat/CTAB/clut/snd/OFxx resources), a linear framebuffer canvas,
   XTimer (host clock), RNG, Sound (later). Unimplemented imports log + stub-return 0;
   implement iteratively by watching the call trace (the method that cracked the blitter).
3. Call the module `main` factory → module object; run its init; then loop calling the
   module's (virtual) `DoBlankScreen` once per simulated tick, reading the canvas
   framebuffer out each frame.
4. Frames go to the SwiftUI/Metal window (live) — one engine drives all 22 A4gm modules.

Open questions to resolve empirically via trace: exact `main` call protocol
(args/return, C++ static-init via `__register_global_object`/`__initialize`), canvas
creation path (`CreateTheScreenCanvas`), and which of the 297 imports are actually hit.

## Phase-2 progress: `adhost` runs real module code (reconnaissance)
`tools/adhost` links the real module PEF + real `adxpl510` into one emulated
address space and executes the module's `main`. Findings from live tracing:

- **Emulator gaps filled** (in the scratchpad resource_dasm fork): the PPC32
  emulator had the *entire* FP unit stubbed. Implemented 40 FP ops (lfs/lfd/
  stfs/stfd±u, fmr/fneg/fabs/frsp/fctiw(z), fadd/sub/mul/div/sqrt/madd/msub/
  sel/cmpu/cmpo and the single-precision `...s` variants, mffs/mtfsf), plus
  `mfcr`/`mtcrf`. Also made `MemoryContext::set_symbol_addr` tolerant of
  multiple symbols sharing one address (C++ aliases/vtables/TVectors).
- **Module `main` ABI confirmed** from disassembly (FT `main` @ code 0x12330):
  `main(r3=arg0ptr, r4=arg1, r5=short selector, r6=arg3)`. First call: a guard
  (`0x12A08`) returns 0 → runs one-time init `0x121D4` (publishes the module's
  params into adxpl510 engine globals `theirParams/theFrame/theirResult/...`,
  which are DATA imports resolved to real adxpl510 export addresses), then
  dispatches the selector through a jump table at **module TOC + 10232** with up
  to 8 messages. On later calls the guard returns 1 → skips init, branches to
  `0x125CC` (the per-message body).
- **The wall:** the module's init reads engine context objects (the "frame",
  canvas, params) via adxpl510 globals. With no faceplate those are null. A
  zeroed low-memory page (0..0x4000, authentic Mac low-mem) lets optional null
  lookups (e.g. the `'ADdl'` tagged-data lookup at adxpl510 `0x10EC0`) no-op
  gracefully, but the module then needs *real* frame/canvas contents — faking
  them yields wild pointers. So the next phase must build the real engine
  context the way the faceplate does.

### Next phase (build order)
1. RE the AD4 faceplate call protocol (what it passes as arg0/arg1/selector/arg3
   and, crucially, the layout+construction of the frame/canvas/params objects and
   the message order: create → setup → per-frame blank → close). Candidates:
   `After Dark 4.0 Library` (SHDL, 68K) and `After Dark 4.0 Faceplate` (FACE).
   Alternative: drive via adxpl510's own exports — construct the module object
   through the right selector, then call the engine's `CreateTheScreenCanvas` +
   `PerformBlank`/`DoBlankScreen` against a host framebuffer.
2. Implement the Mac Toolbox subset the engine actually calls once messages flow
   (Memory Mgr done; add Resource Mgr serving the module's RLEP/Rdat/CTAB/clut/
   snd, a GWorld/pixmap canvas, QuickDraw setup, XTimer, RNG).
3. Read the canvas framebuffer each frame → SwiftUI/Metal window.

## Phase-2 deep dive: the init handshake (why a bare `main` call can't work)
Tracing `main` selector 0 (INIT) pinned the exact bootstrap contract:

- **Exception model = setjmp/longjmp.** `0x12A08` is `setjmp(jmpbuf)` (saves
  LR/CR/SP/TOC, `stmw r13`, and f14–f31 via `stfd`), `0x12A7C` is
  `longjmp(jmpbuf,val)`. After Dark's `XException`/`theException` unwinds through
  these. (Implementing the FP unit + `mfcr`/`mtcrf`/`mffs`/`mtfsf` was a
  prerequisite just to run setjmp.)
- **Module lifecycle** — `main`'s 8-selector table (module TOC+0x27F8):
  sel 0 = INIT (`0xC65C`: `setjmp`, then `operator new(88)` + ctor `0xB39C` →
  builds the module object, stored at engine global `[r31]`); sels 1/2/3 call
  virtual methods vtable+0x08/0x0C/0x10; sel 7 dispatches mouse/button/key on
  `event[+0x36]` → vtable+0x14/0x18/0x1C/0x20/0x34; sels 4/5/6 are weak no-op
  `blr` stubs. So the per-frame draw is one of the vtable+0x08/0x0C/0x10 virtuals
  (map via `__vt__15AfterDarkModule`).
- **The hard gate: the `'ADdl'` frame blob.** Before dispatch, `main`'s one-time
  init (`0x121D4`) looks up tagged data `'ADdl'` in the engine `theFrame` object
  (`adxpl510` `0x10EC0`: needs `theFrame[+0x0E]` bit 17 set and a table at
  `theFrame[+0x28]` of `{fourcc,value}`). It then requires the returned blob's
  first `short` to be **≥ 768** — otherwise it sets error 1284 and **`longjmp`s
  out** (this is exactly the failure observed). `'ADdl'` is **not** a module
  resource (the module has `ADgm` = a 68K entry stub, `TMPL`s only for the
  OFst/OFmm/OFtb sprite-composition tables); it is **synthesized by the faceplate**
  and handed to the module via the frame. So bootstrapping requires reproducing
  the faceplate's frame/`'ADdl'` construction.

### Consequence for the build
The faceplate (`After Dark 4.0 Faceplate`/`After Dark 4.0 Library`) is pure 68K
and builds the `theFrame` context (incl. the `'ADdl'` blob) before ever calling a
module. Options: (a) RE that 68K frame-construction to synthesize a valid
`theFrame`+`'ADdl'` in the host; (b) run the 68K faceplate itself under the 68K
emulator up to the module hand-off. Either is a real sub-project; (a) is narrower
if the module's own consumption of `'ADdl'` pins the layout.

## Phase-2 milestone: past the bootstrap gate, INIT runs real engine code
Recovered the exact `main` arg convention by tracing: `main` publishes engine
global `theirParams = arg3 (r6)`, and the required `'ADdl'` tagged-data lookup
runs on `theirParams`. So the host passes a synthesized frame/param block as
**arg3** carrying a tag table `{u16 count; {fourcc,ptr}...}` with an `'ADdl'`
entry whose blob starts with a version `short >= 768`. With that in place the
`longjmp` gate clears and INIT executes real engine code — observed calls:
`SetResLoad`, `Count1Resources`, `Gestalt`, then an `adxpl510` env-check.

Two more emulator bugs fixed along the way (see emulator-patches):
- `lwz rX,[rX+off]` was wrongly rejected (the `ra==rd` restriction only applies
  to the *update* form `lwzu`).
- `xori`/`xoris` were unimplemented.

Remaining for a live frame (iterative bring-up): satisfy the engine environment
checks (`AfterDarkExists`-style, which read After Dark globals), implement the
**Resource Manager** (serve the module's RLEP/Rdat/CTAB/clut/OF*/snd from its
resource fork so the object ctor can build sprites), a GWorld/pixmap **canvas**,
`XTimer`, and RNG; then read the canvas each frame to the window.

## Phase-2 MILESTONE: the real module runs through full construction
After fixing the critical `lmw` endianness bug and implementing the missing
PPC ops (mul/div/logical), and providing a minimal Toolbox (Memory Mgr, timers,
Gestalt('aYmm')->version 768 so `AfterDarkExists` passes, and a **Resource
Manager** serving the module's resource fork via resource_dasm's `ResourceFile`),
the real Flying Toasters module executes its actual code through:
`PortableModule` ctor -> `XArtDatabase`/`XSoundDatabase`/`XTimer` ctors -> module
RNG -> art-database init. Proof it's the genuine module: its own destructor emits
`dprintf("Deleting m_pToasterControl")`, `m_pFlyingAreaBackground`,
`m_pScreenCanvas`, `m_pNoiseMaker`, etc.

### Next concrete blocker: the companion resource database
`IXArtDatabase` reads `STR# 1000` and opens the path
**`:Module Resources:After Dark 4.0 Library`** as a second resource file (the
module's `dll#` lists `LIB510_admod/ansi/Audio/Canvas/Coord/File/Glue/Memory/
module/R...` sub-libraries; `STR# 5` has "Could not find Resource Database file").
So the host Resource Manager must also serve the `After Dark 4.0 Library` resource
fork (and honor UseResFile/OpenResFile so lookups fall through to the right file).
After that: create the screen canvas (GWorld/pixmap), finish INIT, then drive the
per-frame draw virtual and read the canvas to the window.

### Emulator/Toolbox status (adhost)
- PPC emulator now complete enough to run the engine: FP unit, mfcr/mtcrf,
  mul/div, xor & friends, and the lmw endianness fix (all in emulator-patches).
- Toolbox implemented: Memory Mgr (NewPtr/NewHandle/Handle sizes/BlockMove),
  Resource Mgr (Get1Resource/GetResource/Get1IndResource/Count1Resources/
  GetIndString/GetResInfo/SizeResource/CurResFile...), Gestalt, Microseconds/
  TickCount, p2cstr/c2pstr. Env: ADRSRC=<module.rsrc>, ADENGINE=1 (name engine
  calls), ADTRACE=1 (init handshake), ADR30/ADTOC (diagnostics).

## Phase-2: the real per-frame draw code executes (PerformBlank/DoBlankScreen)
Serving the companion `After Dark 4.0 Library` resource fork as a second open
resource file (host Resource Manager now chains module fork + library fork, with
UseResFile/CurResFile refnums) got INIT past the art-database resource-linking.
The module then constructs its `GraphicsState`/`XRegion`, sets up the reference
canvas, and reaches **`DoBlankScreen` -> `PerformBlank`**, issuing the real screen
blank via QuickDraw: `SetOrigin`, `GetClip`/`ClipRect`, `ForeColor`, `FillRect`,
`SetClip`. It runs ~79 host OS calls deep before aborting with error 513 (a
*canvas* error, STR# 2 "CanvasErrors").

### Final blocker: a real PixMap-backed screen canvas
`MacScreenPixels`/`MacScreenCanvas` read the GrafPort's depth+bounds
(`GetDepth`/`GetBounds`) and QuickDraw draws into it. With GetPort/GWorld stubbed
to 0 the canvas is degenerate -> err 513. Remaining work: build a valid CGrafPort
+ PixMap (baseAddr -> a host framebuffer, rowBytes, bounds e.g. 512x384 or
640x480, pixelSize 8, a clut) + GDevice, return it from GetPort/GetMainDevice/
GetGWorldPixMap, and implement the few QuickDraw draw ops (FillRect = clear; the
sprite blit already goes through adxpl510's XCanvas plot path we drive in adblit).
Then read the framebuffer each `DoBlankScreen` tick = the live frame. Disassemble
`MacScreenPixels` init to get the exact PixMap fields it requires.

Toolbox added this pass: QuickDraw region ops (NewRgn/DisposeRgn/RectRgn/...),
multi-file Resource Manager. adhost env: ADRSRC=<module.rsrc>, ADLIB=<library.rsrc>.

## Phase-3: the real module runs end-to-end (main returns 0, full animation)
Four fixes broke the dam and the Flying Toasters module now runs its whole
lifecycle — INIT -> heap/canvas/palette/sound setup -> DoBlankScreen -> the
sprite animation loop (rleNewBlit compositing 40x) -> clean teardown -> `main`
returns **r3=0** after ~6676 host OS calls (up from ~1205 stuck in construction).

Blockers cleared this pass:
1. **XNoiseMaker::CalcMemNeeded returned a ~3.5GB garbage heap size.** Its 2nd
   loop sizes *external* file-backed sounds: GetResource('mUsk',id) -> FSMakeFSSpec
   -> FSpOpenDF/FSpOpenRF -> GetEOF. Our stubs returned noErr so the module read
   uninitialised EOFs. Fix: FSMakeFSSpec/FSpOpenDF/FSpOpenRF/GetEOF return
   **fnfErr (-43)** so the module gracefully skips absent external sounds (the 13
   embedded 'snd ' resources are sized by the 1st loop). Heap size -> ~0x237782.
2. **Two emulator store bugs (endianness):** `stwx`/`stwux` wrote host
   little-endian (`write<uint32_t>`) instead of `write<be_uint32_t>`. A stwx/lwzx
   round-trip byte-reversed pointers (0x002C677C -> 0x7C672C00) -> wild-address
   fault in XNoiseMaker::Open.
3. **Two emulator indexed-access bugs (spurious base update):** `lhax` and `stbx`
   wrongly executed the *update* form (`regs.r[ra] = EA`), corrupting the base
   register. `lhax r4,[r25+r31]` silently incremented r25 (the sound-ID list
   base) each iteration, so LoadByID walked off the list and hit id 0 -> err 260.
   Plain lhax/stbx must not write ra. (All four fixes are in emulator-patches.)
4. Implemented the temp/zone Memory Mgr (TempNewHandle actually allocates,
   TempMaxMem/TempFreeMem/MaxBlock/PurgeSpace, GetZone/SetZone), a real
   **NewGWorld** (offscreen CGrafPort+PixMap+buffer), GetGWorldPixMap/LockPixels/
   GetPixBaseAddr, and a working **CopyBits** (8-bit, resolves &CGrafPort.portBits
   via the 0xC000 portVersion, src/dst-rect pixel copy).

### Remaining: present the composited frame to the host framebuffer
INIT (selector 0) *is* the whole run (init+animate+teardown); later selectors
(1=vtable+0x08, 2=+0x0C, 3=+0x10) can't be driven afterward because the module is
already torn down. During the loop, sprites decode via rleNewBlit into per-sprite
bitmaps and composite into the offscreen sprite canvas (MacOffScreenCanvas over a
NewGWorld), but the offscreen->screen present (CopyPixels/CopyTo -> CopyBits with
dst=screen) only fires ~3x at init, never per animation frame — so g_fb stays
blank. Next: disassemble the module's Background draw/present path to find the
per-frame present (or the gate that suppresses it), or intercept the sprite
canvas buffer directly. adhost env added: ADFBDUMP (PPM of g_fb + gworlds),
ADSNAPBLIT=N (snapshot every Nth rleNewBlit), ADFRAMES/ADDRAWSEL/ADFRAMEDIR.

## Phase 4 — module runs per-frame; sprite spawn gated on control values

Corrects the Phase-3 "INIT is the whole run" claim. With the module (`FT.rsrc`)
and library (`AD4Library.rsrc`) resource forks loaded via **ADRSRC/ADLIB**, the
selector protocol is:

- **sel 0 = INIT**: construct, load 185+118 resources, decode 40 RLE sprite
  frames, capture the screen background (3 CopyBits screen->offscreen), set up.
  Returns 0 in ~6672 OS calls. (Running without ADRSRC makes INIT bail at ~175
  calls with err 0x104 — that was the false "regression".)
- **sel 2 = DrawFrame**: a *real, repeatable* per-frame render (~86 OS calls
  each). Renders the `Background` (FillRectangle with the FT backdrop = colour
  index 0 = black, so the flying-area buffer reads as "all zero" but is correct),
  runs the 333-entry CompoundSprite update loop, and presents (1 CopyBits/frame,
  flying-area -> screen). Fully drivable frame after frame; state persists.

### Fixes this phase
1. **GetMonitorInfo screen size (root of the 0x0-canvas bug).** The engine sizes
   every offscreen GWorld from `GetMonitorInfo__PortableModule`, which reads the
   emulated GrayRgn / GDevice monitor table — degenerate here, so it returned a
   0x0 rect and all GWorlds came out 1x1. Fix: host-trap the function's
   convergence point (module-region pc `0x1000E2E4`, where r29 = the output XR)
   and overwrite the XR with the true framebuffer extent. **XR layout is
   {left,top,right,bottom}** (confirmed from the E1B8 store sequence). GWorlds now
   size correctly to 512x384. (Do NOT force it at IMacScreenPixels entry — that
   corrupts the coords its own LocalToGlobal calls then transform.)
2. **CopyBits out-of-bounds crash.** The 8-bit copy loop clamped only lower
   bounds; the first full-size composite ran a rect past the source buffer and
   faulted (`address not within any arena`, faultAddr ~0x467C). Fix: clamp sx/sy/
   dx/dy against each pixmap's real extent (sExtW/H, dExtW/H).
3. **Emulator: `subfic` was an unimplemented stub** (threw "unimplemented opcode
   20060050"). Implemented as `rD = ~rA + EXTS(SIMM) + 1` with `XER[CA]` from the
   carry-out. In emulator-patches now (patch is 735 lines).
4. **Control values were all zero -> zero sprites.** Every module reads its
   control-panel sliders via `GetControlValue(i) == (*(adxplTOC+0x3A8))[i]`
   (16-bit shorts; the array lives at module+0x13A08, i.e. `theFrame`). Our host
   provides no faceplate/preferences so they were 0, and FT's DrawFrame spawn math
   (module fn 0xBF28 -> C030+, float-probability gated) produced 0 toasters. Fix:
   after INIT, seed the control array (default 50, **ADCTRL="v0,v1,..."** to
   override). With this the per-frame AddSprite/config calls (module 0xAD38/0xCE10)
   fire and sprite indices advance.

### Still open: spawned sprites are not blitted
Even with controls seeded and the clock advanced (**ADUSPERCALL** overrides the
per-call microsecond step; XGetMilliseconds->Microseconds), the flying-area buffer
stays empty: the 333-entry CompoundSprite loop only ever calls
`UpdateSpriteMovement`/`NextSequence`/`HideSprite`, and `MaybeDraw__Sprite` /
`DrawFrame__16CompoundSequence` / `DrawFrame__15a64KRLESequence` are **never**
invoked. So sprites are moved and hidden but never shown. Likely causes to chase
next: (a) FP math in the spawn/position path (C140+ uses lfd/lfs/fsubs/fdivs/
fmuls over TOC float constants) putting sprites off-screen or into an invalid
state; (b) `IsValidFrame__16CompoundSequence` always false (no valid animation
frame -> permanent hide); (c) sprite-list/active-queue management. The module's
DrawFrame virtual is at module+0xBE40 (vtable[0x0C], a TVector at module+0x145C4);
its main animation worker is fn 0xBF28. Background renders correctly; only the
sprite layer is missing.

New adhost env this phase: **ADRSRC/ADLIB** (resource forks — required),
**ADSCANMEM** (report allocated blocks that are >5% non-zero — locates real pixel
buffers), **ADCTRL** (seed control values), **ADUSPERCALL** (clock step),
**ADSPAWN/ADVTBL** (spawn-path + vtable tracing), **ADNOSCREENFIX** (disable the
GetMonitorInfo XR override).

## Phase 5 — sprite draw gate fully localized (sprites never activated)

Traced the exact reason no toaster is composited, via runtime vtable/field probes
(adhost env **ADDRAWTRACE**, plus ADTALLY/ADFORCEFRAME). Chain, per sel-2 frame:

- The 333 animated CompoundSprites (vtable 0x10045198; e.g. 0x69275C, seq[+0x40]=
  0x3DC020) run `NextSequence -> NextSubSequence -> MoveThroughRelativeFrame ->
  UpdateSpriteMovement -> HideSprite` and nothing else. The draw methods
  `MaybeDraw__Sprite` (0x100025C0) and `DrawFrame__16CompoundSequence` (0x100020F0)
  are **never** called during sel=2 (they ARE during INIT — that's RLE frame
  decode, not screen compositing).
- `MoveThroughRelativeFrame` (0x100202CC) sets `[sprite+0x44] = r4` (the target
  frame). It is always called with **r4=0**, so every sprite is pinned at frame 0.
- The draw dispatcher gates on `[sprite+0x44] > 0`; frame 0 => skip. `IsValidFrame`
  (0x1001E5BC) requires `0 < frame <= seq[+0xBE]`, so frame 0 is always invalid.
- r4 comes from `GetSequenceFirstFrame` (vtable+0x11C, 0x10020A5C) called with the
  sprite's sub-sequence index `[sprite+0x4E]=0`; `GetSequenceFirstFrame(...,0)`
  returns 0. The sub-sequence index is 0 because the sprite's sequence **queue is
  empty**: `NextSequence` (0x1001EE34) is *only ever* called with arg 0 (the
  reset/hide path, FT module fn 0xDA90 @ lr 0x3000DAB8), never with a real index.
- So sprites are perpetually reset+hidden and never activated. Confirmed by
  ADFORCEFRAME (forcing frame>=1 into MoveThroughRelativeFrame still produced no
  pixels — the draw dispatch isn't reached, so the gate is upstream of drawing).

Two distinct sprite object arrays exist and must be reconciled next session:
- 248-byte **CompoundSprite**s (0x69275C…, seq=0x3DC020, empty queue) — animated
  in the sel-2 loop, stuck at frame 0.
- 126-byte **Background sprite records** (0x3F42xx…, matches MixToSaveBackCanvas's
  `mulli ×126`) — these DO carry queue data (e.g. [+0x50]/[+0x54]=81, 159) written
  during INIT via `AddSequenceToQueue`/`AddSequencesToQueue` (0x10002120/0x2128/
  0x2130), but their seq pointer field reads 0. 213 `AddSprite__Background`
  (0x10002018) calls happen, all during INIT.

`Background::MixToSaveBackCanvas` (runtime 0x1001A970 — NOTE: disasm export-table
offsets are TVector addrs, not code; trust the runtime symbol map) mixes the
background *layers* (gated on bg[+0x16] canvas & bg[+0x12] source), not sprites.

### The one remaining unknown
What activates a Background sprite record into a playing CompoundSprite (assigns
its CompoundSequence + non-zero starting frame). It is gated somewhere in FT spawn
worker fn 0xBF28 (float-probability path C10C+) or the sprite-system processors
0xA890/0xCFF8 (called from DrawFrame when module obj+0x26/+0x2A != 0). Seeding
control values (Phase 4) got AddSprite firing but not activation. Next: hook
0xA890/0xCFF8 and the write to a sprite's seq[+0x40]/sub-seq to find the activation
call and its unmet precondition. Emulator FP is functional (module runs), so the
gate is a missing state/precondition, not an arithmetic bug.

### Phase 5 addendum — the real draw fn is ArtSprite::MoveSpriteAndFrame
Resolved the actual sprite-draw dispatch (FT uses **ArtSprite**, not the MaybeDraw
path): `UpdateSpriteMovement__CompoundSprite` (0x100205DC) branches on
`[sprite+0x44]` (current frame): if !=0 it calls vtable+0x160
`UpdateSpriteGround` then vtable+0x74 `MoveSpriteAndFrame__9ArtSprite`
(0x1001A190); if ==0 it calls vtable+0x34 `HideSprite`. So the frame gate is the
whole story — every sprite has frame 0 so every sprite hits HideSprite.

Confirmed by experiment (**ADFORCEFRAME=1** forces MoveThroughRelativeFrame's
target frame to 1): MoveSpriteAndFrame then fires for all sprites — but still zero
pixels. Reason: MoveSpriteAndFrame only calls `seq.vtable+0x20`
`GetFrameRect__16CompoundSequence` (0x1001DB78) twice to compute/offset the frame
rect around the sprite's XPt `[sprite+0x4A]`; it does NOT blit — the blit is
deferred to a dirty-rect batch. And critically, all of this is happening inside the
per-frame **reset path** (module fn 0xDA90 → NextSequence(arg 0)), not a real draw
pass, so the sprites have neither an active sequence nor a valid position.

Bottom line: the pipeline is complete and reachable; the one missing thing is
**sprite activation/spawn** — assigning a sprite a running sequence + fly-in
position so its frame advances 1..N and it enters the real (non-reset) draw path.
That is the FT spawn state machine (fn 0xBF28 + sprite systems obj+0x26/+0x2A via
0xA890 clear / 0xCFF8). All draw addresses/gates are now known; next session should
force-activate one sprite (assign seq + frame + XPt, drive the non-reset path) to
confirm a toaster blits, then trace why the natural spawn never activates.

## Phase 6 — compositing buffers definitively black; run-recipe fix (2026-07-11)

**Run-recipe correction (important):** adhost's `max_calls` default is 2000, which
truncates INIT (needs ~6672 OS calls) — it stops mid-INIT returning a garbage r3.
Must pass a high maxcalls arg: `./adhost ADShared.pef FT.module 2000000`. Also the
real per-frame DrawFrame is selector **2** (`ADDRAWSEL=2`); default draw_sel=1 is a
different selector. Known-good: INIT sel0 = 6672 calls ret 0; each sel2 = 86 calls
ret 0; 3 gworlds; ~1 CopyBits/frame once the clock advances.

**Spawn timer clock chain fully resolved — and ruled out as the gate:**
FT `fn0xBF28` glue `0x12D54`/`0x12D84` → `XTimer::GetMilliseconds` (engine
0x1002D024) → `fn0x2D0F0` → `fn0x3D310` = **Microseconds (import 247)**. So the
spawn clock is controllable via `ADUSPERCALL`. `fn0xBF28`'s FIRST gate (`bl 0x12D84`)
is actually **GetCapsLockChange** (a caps-lock easter-egg/pause path), returns 0, so
the `[TOC+0x3D0]` timer-store block never runs — irrelevant to normal spawning.
Cranking `ADUSPERCALL=50000..100000` over 60–80 frames: still 100% black. Seeding
all control values to 100 (`ADCTRL=100,...`, GetControlValue=TOC[0x7C]=import 81):
still 100% black. So neither the timer nor the control-value `>=75` trajectory gate
(`fn0xBF28` @0xC190) is the activation trigger.

**Decisive finding — every compositing buffer is black:**
Per-frame `CopyBits` copies `0x378000 (512x384) -> 0x3A8000 (512x384)` — the module's
OWN GWorlds, NEVER the host screen fb at `0x48000`. Added `snap_all("final")` +
`ADDUMPADDRS` to adhost and dumped all buffers through the CLUT after 60 frames:
`g_fb`, all 3 GWorlds, and both `0x378000`/`0x3A8000` = **0 nonzero (fully black)**.
Meanwhile `ADSCANMEM` shows the toaster/toast ART is fully decoded in heap blocks
`0x69xxxx..0x73xxxx` (96–98% nonzero). Conclusion: art is loaded, but sprites
composite as BLANK — `MixToSaveBackCanvas` reports "-> DRAWS" for group 0x3758E0 yet
writes zero pixels, because each iterated sprite's current-frame bitmap is null/0.
This re-confirms and hard-localizes the Phase-5 diagnosis: **sprite frame index never
advances past 0** (art→sprite→current-frame linkage is 0). The host↔g_fb disconnect
(module composites into 0x3A8000, host dumps 0x48000) is a secondary wiring issue to
fix later, but is NOT why the screen is black — the module's own buffers are black too.

**Correction:** `fn0xAD38` (called from `fn0xBF28`@0xC0F4) is NOT AddSprite — it
resets sprite system A (`0xA890`) and sets mode flag `[obj+0x124]`. The `fn0xBF28`
main path (0xC030+) is the toaster-manager trajectory/reset, not sprite population.

**Next:** find the sprite-population + frame-advance path (pool 0x69275C, 126-byte
Background records in group 0x3758E0). Trace `MixToSaveBackCanvas`: for each sprite it
iterates, which field is the current-frame bitmap, and what code sets it nonzero
(activation) — and why that code path isn't taken headless. adhost debug env added
this phase: `ADCBLOG` (CopyBits src/dst), `ADDUMPADDRS`/`ADDUMPW`/`ADDUMPH`, and
`snap_all("final")` auto-dump when ADFBDUMP set and ADFRAMES>0.

## Phase 6b — blit pipeline fully mapped: per-frame sprite draw is missing (2026-07-11)

Engine-call trace (ADENGINE) split by selector phase is decisive:
- **INIT (sel 0):** `rleNewBlit` (engine 0x10000004) fires **40×**, EVERY call with dst
  buffer r4=**0x003DE0AC** (a constant) -> INIT builds a sprite ATLAS at 0x3DE0AC by
  RLE-decoding 40 bitmaps. `AddSprite__10Background` (0x1001AA0C) fires **213×**;
  `CalcBlitCondition__11RLESequence` (0x1002B384) 40×.
- **DRAW (sel 2), per frame:** ONLY `MixToSaveBackCanvas__10Background` (0x1001A970),
  2×/frame. **ZERO** rleNewBlit, AddSprite, MaybeDraw__6Sprite, CompoundSequence::
  DrawFrame during DRAW.
- So per frame the module does: erase (fn0xB2F8 -> fn0xB1F8 -> `FillRectangle__9MacCanvas`
  via vt+0x74, confirmed live) on the group canvases (0x3759B8, 0x375BF8) + module
  screen canvas 0x30013E80; then MixToSaveBackCanvas (composite, emits 0 px); then one
  full-canvas CopyBits 0x378000->0x3A8000. **No per-sprite blit ever happens during
  animation.** Sprites are in the atlas + AddSprite'd, but nothing draws their current
  frame each frame -> the erased black canvas propagates to screen. This is THE gap.

Module DrawFrame (fn0xBE40) leading calls: bl 0x12C94 (TOC[0x14]), bl 0x12D0C
(TOC[0xB0]), bl 0x12D24 (TOC[0xB4]) -- unresolved engine fns, one likely the sprite-
draw/animate trigger; then fn0xB2F8 x2 (erase); fn0xA890/0xCFF8 (reset); fn0xBF28
(toaster mgr). Subagent (session 5cce8c88, agent a1e3ec9eb8d86279d) resumed to resolve
those 3 glue calls + why MixToSaveBackCanvas emits 0 px with 213 sprites + the minimal
state to force one visible toaster via the module's own draw code.

Subagent Phase-6 address map (verified vs known NextSequence 0x1EE34 /
MoveThroughRelativeFrame 0x202CC): MixToSaveBackCanvas 0x1A970, MaybeDraw(Sprite)
0x229F4, MoveSpriteAndFrame(ArtSprite) 0x1A190, CompoundSequence::DrawFrame 0x1DA60,
GetSequenceFirstFrame 0x20A5C, QueueSequence 0x1EFD8 (sets +0x52 start,+0x58 target),
MoveIntoNextSequence 0x203A8, PlaySequence 0x1F244, AddSequenceToQueue 0x20E24. FT
imports AddSprite/MaybeDraw/CompoundSequence+a64KRLESequence::DrawFrame/IsValidFrame +
sequence builders, but NOT the CompoundSprite state machine (QueueSequence/PlaySequence/
NextSequence/etc). Sprite fields: +0x3C visible, +0x40 frame-source, +0x44 current
frame (draw gate), +0x52 start flag, +0x58 target seq(-1=none). adhost env added:
ADB1F8 (per-sprite-erase trace), ADDRAWFNS (engine draw-fn-by-name trace; note: buggy,
misses fns ADENGINE catches -- use ADENGINE instead).

## Phase 6c — the sel=2 path does NOT draw sprites; render pass is elsewhere (2026-07-11)

Deep trace of the module DrawFrame (sel 2 -> vt+0x0C -> fn0xBE40) call tree, confirmed
live:
- 3 leading glue calls = monitor-blank boilerplate: TOC[0x14]=DoBlankScreen (import 13),
  TOC[0xB0]=GetDeepestMonitor (105), TOC[0xB4]=BlankOtherMonitors (110). NOT sprite draw.
- `MixToSaveBackCanvas__10Background` (0x1A970) = a pure canvas->canvas COPY gated only on
  [Bg+0x16] (dest save-back) and [Bg+0x12] (src mix) non-null; reads NO sprite fields. It
  is Background vtable slot [vt+0x78], called once/group by fn0xB2F8.
- `fn0xB2F8` = the ERASE-AND-RESTORE half of dirty-rect animation: iterate sprites, get
  bounds (vt+0x60), erase each old rect via fn0xB1F8=`FillRectangle__9MacCanvas` (vt+0x74),
  then MixToSaveBackCanvas. It does NOT blit sprite frames.
- `fn0xCFF8` (sprite sys B) calls `[vt+0x84]`=`NextSequence__14CompoundSprite` on each
  CompoundSprite (e.g. toaster 0x6F04C8) EVERY frame. NextSequence ignores its arg and just
  RESETS sequence-stepping state (zero +0x50/54/56/5A, vt+0x130, vt+0x13C). fn0xCF28 wraps
  fn0xCFF8. fn0xA890 similarly resets sys A.
- DrawFrame gates ([mod+0x1E]=grp 0x3758E0, +0x22=grp 0x375B20, +0x26=sys 0x37638C,
  +0x2A=sys 0x6EDF34) are ALL nonzero; fn0xA890/fn0xCFF8/fn0xB2F8 all run. Not a gating bug.

Net: during sel=2 the module ERASES + RESETS + COPIES but never composites sprite frames
(MaybeDraw=0, rleNewBlit=0, CompoundSequence::DrawFrame=0). ALL sprite drawing
(rleNewBlit x40 -> atlas 0x3DE0AC, AddSprite x213) happens during **sel 0 INIT**, whose
handler (0x123CC) at 0x1242C calls fn0xC65C -> fn0xB7A8. fn0xB7A8 is a big single-pass fn
(setjmp 0xBCDC + longjmp-cleanup loop 0xBC34..0xBC74; the body runs ONCE, not a frame loop).
Selector dispatch: sel1->[vt+0x08], sel2->[vt+0x0C]=fn0xBE40, sel3->[vt+0x10], sel4->fn0xC768,
sel5->fn0xC76C, sel0 tail->fn0xC770.

HYPOTHESIS (testing via selector sweep): the real per-frame render is NOT sel=2. Either a
different selector runs the composite, or (more likely) the After Dark ENGINE/host run loop
is responsible for calling the Background sprite-composite each frame (MaybeDraw ->
CompoundSequence/a64KRLESequence::DrawFrame -> rleNewBlit into [Bg+0x12]) AFTER the module's
per-frame state update -- and adhost never does that. Subagent Q4 recipe to force one visible
toaster: on a Background sprite set +0x44>=1 (current frame) & +0x3C=1 (visible), Invalidate
its rect, then call the engine composite (DrawAboveSprites import 135 / FillMixArea import 152)
before the existing MixToSaveBackCanvas. adhost env added: ADDF (DrawFrame gate + sub-call
trace).

## Phase 6d — no selector draws; atlas format not yet understood (2026-07-11)

Selector sweep (sel 1-6, ADENGINE, draw-phase rleNewBlit count): ALL ZERO. Definitive:
**no module selector performs the per-frame sprite composite.** The engine/host run loop
must drive it (Background composite: MaybeDraw -> DrawFrame -> rleNewBlit into [Bg+0x12]),
which adhost does not replicate. This is the crux to fix.

Atlas investigation (side result): the 40 INIT rleNewBlits all target ONE canvas struct
at r4=0x3DE0AC. Struct header bytes (48): 00010037 80000000 0200003D E0F80000 00000180
02000000 00000000 00000200 01800000 00080000 00FF1004 0F681004. Parse: rowBytes=0x0200(512)
at +0x08, baseAddr=0x003DE0F8 at +0x0A, dims 0x200x0x180 (512x384), depth 8. BUT dumping
0x3DE0F8 as 512x384x8 via the screen CLUT = NOISE at every stride tried (512/64/96/40),
with faint structured horizontal bands. So either (a) wrong palette (module XPalette, not
screen g_clut), (b) non-linear/packed pixel format, (c) baseAddr parse wrong, or (d) stale
post-INIT content. **The RLE-decode -> pixel format is NOT yet understood** -- blind
compositing won't yield recognizable output until it is. rleNewBlit entry = engine
0x10000004; args r3=RLE-source, r4=dst-canvas(0x3DE0AC), r5~=r3 (adjacent mask/pos).

adhost env this phase: ADDUMPADDRS/ADDUMPW/ADDUMPH (arbitrary-addr CLUT dump), ADSNAPBLIT
+ADRAWADDR/ADRAWLEN (raw index dump during blit). NOTE each adhost run ~40s (full INIT);
avoid multi-run width sweeps -- dump raw once, reshape offline.

## Phase 6e — host can call engine fns; Validate needs a draw-array that's null (2026-07-11)

BIG infra win: added `call_code(code,toc,args)` to adhost — can now invoke ANY engine C++
method from the host (engine TOC=0x10048000 from adxpl510:__initialize TVector). Verified:
called FillMixBuffer(0x1001B774)/Validate(0x1001B8C0)/MixToSaveBackCanvas(0x1001A970) on
the Background groups — all run cleanly, no crash. adhost env: ADVALIDATE="bgA,bgB"
(+ADPOKESPRITE/ADPOKERECT/ADFILLMIX/ADVALFN/ADMIXFN/ADNOFILL/ADNOMIX), ADROWDESC (row-table
pixmap dump), ADPAL.

All 213 INIT AddSprites target Background **0x3758E0** (=[mod+0x1E], the group I pass to
Validate). Validate (0x1B8C0) authoritative logic: count = `lha [Bg+0x40]` (=0xD5=213 OK);
sprite array = `lwz [Bg+0x3C]` then `lwzx [arr + i*4]`; per sprite draw gated on visible
`[spr+0x30]!=0`, `[spr+0x32]==0`, dirty `[spr+0x38]||[spr+0x36]`. **BUT [Bg+0x3C]=0 (null)
post-INIT** -> Validate iterates 213 garbage ptrs from low mem, all skipped at the +0x30
check (no crash, no draw). So Validate's flat draw-array is never built.

The 213 sprites actually live in a LINKED LIST of nodes at 0x375940+ (node ~{next, engine
vtable 0x100452FC/0x10044E68, spritePtr 0x6Exxxx/0x69xxxx, ...}); e.g. node@0x375998 holds
sprite 0x6EDE88 + 0x69275C (the 248B CompoundSprites AddSprite received). Background fields
(UNALIGNED 32-bit): [+0x12]=mixCanvas 0x3759B8, [+0x16]=saveBack 0x375A6C, [+0x40]=count(u16)
213, [+0x3C]=drawArray(NULL), [+0x34/0x38/0x5C/0x60/0x70/0x74]=list-node ptrs into 0x375xxx.
Note the Background pixmap descriptor (0x3DE0AC style: +0x0A row-table, +0x0E..0x14 bounds,
+0x22 depth) -- atlas 0x3DE0AC is empty (0 nz) post-INIT (transient scratch).

Next: find the engine fn that walks the sprite linked list and builds [Bg+0x3C] (or draws
the list directly) -- the missing "prepare draw list"/"sort" step the real run loop calls
before Validate. Delegated to subagent (session 5cce8c88, agent a1e3ec9eb8d86279d).

## Phase 6f — MILESTONE: real render pipeline driven into the sprite blit (2026-07-11)

`GetSpriteArray__10Background(0x1001B540)` is the missing step: it walks the sprite linked
list (rooted [Bg+0x34], via list First 0x211E4 / Next 0x211F4) and fills the flat draw-array
`[Bg+0x3C]` (allocs via [Bg+0x38] vtable+0x14), gated on dirty flag [Bg+0x42] (set by
AddSprite). VERIFIED in host: call GetSpriteArray(0x3758E0) -> [Bg+0x3C]=0x7338F8 with 213
sprite ptrs. All 213 are REAL: vtable=0x10045198, frame-source [+0x40]=0x3DC020 (shared
toaster a64KRLESequence), but bounds=0,0,0,0 / visible[+0x30]=0 / dirty[+0x38]=0 / frame
[+0x44]=0 -> a POOL awaiting spawn (position+activation). Validate correctly skips them.

Per-frame engine run-loop sequence (host must replicate; NO module selector does it):
  GetSpriteArray 0x1001B540 -> FillMixBuffer 0x1001B774 -> Validate 0x1001B8C0 ->
  MixToSaveBackCanvas 0x1001A970  (all take r3=Background; engine TOC 0x10048000).
Only Validate consumes [Bg+0x3C]; step 1 was the missing piece.

Poking the pool sprites visible+dirty+bounds(grid)+frame=1 makes Validate ENTER the draw
path (changed bytes 1795->1923) then CRASH: "address not within any arena" faultAddr=0x40D4
pc=E0000068 lr=0x100106B4 (fn000106A4 -> bl 0x3D028). Cause: raw +0x44=1 leaves the
CompoundSprite sequence-stepping state (+0x4E sub-seq, +0x50/54, queue) inconsistent, so
a64KRLESequence::DrawFrame reads a bad frame ptr. Sequence 0x3DC020 layout: +0x00 vtable
0x30014EC4(module), +0x14 RLE src 0x3F43F8, +0x18 atlas desc 0x3DE0AC, +0x1C mix canvas
0x3759B8, +0x20..0x2C 4 frame entries (0x3DD438..), +0x38=0x004F0051.

Sprite (CompoundSprite) fields (vt 0x10045198): +0x18/1A/1C/1E bounds(t,l,b,r), +0x30 vis,
+0x32, +0x36/+0x38 dirty, +0x40 frame-source seq, +0x44 current frame. adhost env:
ADVALIDATE/ADPOKEALL=N/ADPOKEFRAME/ADPOKESW/ADPOKESH, call_code(GetSpriteArray added).

Next: activate ONE pooled sprite via the ENGINE's own method (MoveIntoNextSequence 0x203A8 /
QueueSequence 0x1EFD8 + PlaySequence, or the module's spawn) so frame state is consistent,
+ set an on-screen position, then GetSpriteArray+Validate -> first visible toaster. Delegated.

## Phase 7 — *** BREAKTHROUGH: real Flying Toasters rendered under emulation *** (2026-07-11)

FIRST VISIBLE FRAME. 8 winged toasters composited into the screen framebuffer (0x48000)
by the REAL PPC module code + engine, driven from the host. Validates the ENTIRE stack:
PPC emulator (incl FP/subfic), PEF/CFM load, INIT (RLE atlas decode), sprite system
(Background/CompoundSprite/a64KRLESequence), and the engine composite pipeline.

The crash-free activation recipe (per pooled sprite, all bound to seq 0x3DC020):
  1. probe GetSequenceFirstFrame(sp, n) @0x10020A5C for n=1,2,3.. until nonzero
     -> n=1 INVALID (returns 0), n=2 VALID (returns 2). Use seqIdx=2.
  2. poke XPt anchor: [sp+0x4A]=x (int16), [sp+0x4C]=y.
  3. MoveIntoNextSequence(sp, 2) @0x100203A8  -- sets +0x44 frame, +0x4E sub-seq, bounds
     +0x18..0x1E from the frame at the anchor, AND the blit descriptor/X-shift map (the
     step raw-poking +0x44 skipped -> was the BlockMove null-source crash at faultAddr
     0x40D4). fn000106A4 = a BlockMove RLE per-run copy thunk.
  4. poke visible/dirty: [sp+0x30]=1, [sp+0x32]=0, [sp+0x38]=1 (== Show 0x10022FB8 +
     SetVisible 0x10022FB0).
Then per Background: GetSpriteArray 0x1001B540 -> (FillMixBuffer 0x1001B774) -> Validate
0x1001B8C0 -> MixToSaveBackCanvas 0x1001A970 (r3=Bg=0x3758E0, engine TOC 0x10048000).
Result: pixels land in 0x378000 (mix), 0x3A8000 (saveback/screen GWorld), AND g_fb 0x48000.
8 sprites -> 25799 nz bytes. Image: scratchpad/toast_fb.png (grayscale via g_clut).

adhost env: ADVALIDATE="bg" + ADACTIVATE=N (engine-activate first N pool sprites in a grid)
+ ADSEQIDX + ADFBDUMP (writes .val.fb/.378/.3a8/.color.ppm). call_code(GetSequenceFirstFrame,
MoveIntoNextSequence, GetSpriteArray, FillMixBuffer, Validate, MixToSaveBackCanvas).

REMAINING (this is a host-driven VALIDATION; not yet module self-driving):
- Palette: mixCanvas 0x3759B8 [+0x54]=0 so XPalette not at [+0x5A]; find the real 'clut'
  (module loads its own) for correct colors (toasters=chrome/gray so grayscale ~ok; toast=gold).
- Animation: step frames per tick (PlaySequence 0x1F244 / MoveThroughRelativeFrame) + real
  fly-across-screen movement (the toaster manager fn0xBF28 trajectory).
- MODULE SELF-DRIVE (the real goal): make the module's own spawn (fn0xBF28) activate pool
  sprites over time, OR have the host replicate the full After Dark run loop each frame
  (GetSpriteArray->Validate->Mix + advance) so no per-sprite host poking is needed.
- Generalize the run-loop host to all ~82 modules.

## Phase 8 — *** SELF-DRIVING ANIMATION: real screensaver runs itself *** (2026-07-11)

THE GOAL for FT is reached. Pumping **selector 3** (the real per-frame ANIMATE selector;
sel 2 was a lighter fill mode, sel 1 = full redraw 1020 calls) each frame with:
  - control values seeded (ADCTRL=100..) so GetControlValue(0..3) returns real slider vals
    (density/fps/kind/position), and
  - TickCount advancing fast enough (ADTICKSTEP=6) so the frame-step gate in fn0xCF28
    (elapsed>=[art+0x68] delay) fires, and Microseconds advancing (ADUSPERCALL),
makes the MODULE SELF-DRIVE: toasters spawn over time (black until ~frame 15, then
nz grows 12237->25359->29526, each frame DIFFERENT), fly diagonally "out of the sun",
wings flapping, toast drifting, WITH the iconic "Flying out of the sun" intro text.
NO host sprite poking -- pure module code. Image: scratchpad/s3_f25.png. 177 CopyBits/30fr.

Run recipe (self-driving FT):
  ADRSRC=work/FT.rsrc ADLIB=work/AD4Library.rsrc ADDRAWSEL=3 ADFRAMES=N \
  ADCTRL="100,..(16).." ADUSPERCALL=50000 ADTICKSTEP=6 ADFRAMEDIR=out \
  ./adhost work/ADShared.pef work/FT.module 9000000

Selector map (module main sel via jump table): 0=INIT(6672 calls), 1=full redraw(1020),
2=fill/light draw(86), 3=ANIMATE per-frame(spawn+flap+move+composite, self-driving), 4-7=noop.
The module owns its frame counter (m_pToastersArt, fn0xCF28) and blits explicit frames via
DrawFrame__16CompoundSequence/a64KRLESequence -- it does NOT use the CompoundSprite sequence
state machine (so the Phase-7 host MoveIntoNextSequence path was a valid render but not how
FT animates; Phase-8 sel-3 pump IS how it animates). TrafficControl fn0xBF28 does spawn +
FP trajectory from the sliders. adhost env: ADTICKSTEP (TickCount step).

REMAINING: correct palette (grayscale via g_clut ~ok for chrome toasters); live resizable
macOS window; generalize the sel-3-pump run loop to all ~82 modules (each may use a different
ANIMATE selector -- probe via ADPROBE OS-call/fb-change deltas).

## Phase 9 — GENERALIZATION: multi-module breadth (2026-07-11)

adhost was FT-specific in ONE place: line 146 hardcoded FT's code-section size (0x13200) to
place the module's data section. FIXED: parse the PEF section headers from the module file
(40-byte container header, sectionCount @+0x20 u16be; 28-byte section headers, totalSize
@+0x08) and compute each section's runtime address exactly as PEFFile::load_into does
(skip total_size==0, page-align each non-empty section after the prev). FT unchanged
(sec1=0x30014000); no regression.

Module extraction is trivial: PEF = the file's DATA fork (the file itself); resources =
`<module>/..namedfork/rsrc`. ADShared.pef (engine) + AD4Library.rsrc are SHARED across all
After Dark 4.0 modules (from "After Dark 4.0 Shared"). Modules live under the assets
root (see tools/adfetch) at
`extracted/After Dark 9 v1.0 (9:9:03)/After Dark Files/After Dark 4.0/`
(~22 modules: Art Critic, Bad Dog!, CYb3r W@t, Fish World, Flying Toasters!, Guernsey Madness,
Life & All, Magic Turtle, Marbles!, Messages 4.0, Out 'n About, Points of View, Psycho Deli,
Rainforest, Rock Paper Scissors, Rodger Dodger, Shadow Agents, Slow Burn, Super Guy, Swirling
Magic, Time Flies). More in Deluxe/Ray/Multimodules collections (~82 total).

SECOND MODULE WORKS: "Bad Dog!" -- INIT ok (31839 calls), sel-3 pump renders+animates the
full Mac-desktop scene (dog, folders, Clipboard, trash), 60/60 distinct frames, nz 99K->143K.
Same host, same run loop (ADDRAWSEL=3 + seeded controls + ADTICKSTEP). Image baddog_f40.png.
(Minor: teardown "invalid opcode pc=0" after last frame -- cosmetic, post-render.)

General run recipe for any AD4.0 module:
  cp "<mod>" m.module ; cp "<mod>/..namedfork/rsrc" m.rsrc
  ADRSRC=m.rsrc ADLIB=AD4Library.rsrc ADDRAWSEL=3 ADFRAMES=N ADCTRL=100.. ADUSPERCALL=50000
  ADTICKSTEP=6 ADFRAMEDIR=out ./adhost ADShared.pef m.module 9000000
Batch harness: scratchpad/batch_modules.sh (runs all AD4.0 modules, records init/crash/
copybits/nz + dumps frames). Next: review batch results, handle modules that use a different
ANIMATE selector (probe) or need per-module fixes, then Deluxe collections.
