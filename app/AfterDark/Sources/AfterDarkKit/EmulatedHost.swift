import Foundation
import CoreGraphics
import ImageIO
import Darwin
import ADCShim  // adshm_open_create: ABI-correct variadic shm_open(O_CREAT) wrapper   // addm 571: shm_open/mmap/munmap/ftruncate/poll for the shm transport

// Drives an emulation host (adhost / adhost68k) for one module: spawns the
// process with ADSTREAM=1 so it writes CONCATENATED P6 frames to stdout, reads
// that stream through a pipe, parses each frame incrementally, decodes it to a
// CGImage, and publishes it. NOTHING touches the filesystem — there is no frame
// dir, no polling, no pruning, no cleanup. Pipe backpressure paces the host to
// our read rate; if this process dies the host gets SIGPIPE and exits, so no
// orphan can ever run unbounded (cleanup by construction).
//
// Terminates + respawns if frames stall, and can be restarted with new control
// values. This class is UI-agnostic (no SwiftUI/SpriteKit) so it can be driven
// headless from adrender for verification.
public final class EmulatedHost {
    // addm 569: master switch for duplicate-frame suppression (default on). Flipped
    // off only by the adrender --verify-dedupe A/B to measure the CPU it saves.
    public static var dedupeEnabled = true

    // addm 571: master switch for the zero-copy shared-memory transport (default on).
    // When true the host is spawned with ADSHM=1 + ADSHMNAME and driven in strict
    // GO/F lockstep over shared memory; the P8/P6 stdout stream is kept as a
    // transparent fallback (old host, or shm setup failure). Flipped off by
    // adrender --verify-emulation's P8-fallback pass to exercise the stdout path.
    public static var shmEnabled = true

    // addm 796: the user's Duration preference, in seconds, or
    // ADDuration.forever for "Forever!" (never cycle). Applied by _buildEnv to every
    // spawn from this point on.
    //
    // nil means "nobody expressed a preference" and leaves the shipped behaviour
    // exactly as addm 744 set it (ADCYCLE=1, host default period 60 s). This is what
    // keeps the HEADLESS paths out of it BY CONSTRUCTION: only the app (ADDurationStore)
    // and the saver (ADSaverView.resolveSelection) ever assign this, so adrender and the
    // verification harnesses — which build EmulatedHost directly and never touch it —
    // spawn byte-identical environments to before this change.
    public static var durationSeconds: Int?

    public let module: ADModule
    private let onFrame: (CGImage) -> Void

    private var process: Process?
    private let queue = DispatchQueue(label: "adhost.control")
    private var running = false
    private var settings: [String: Int]
    private var lastFrameAt = Date()
    // First-frame grace: a module still INITIALIZING is not hung. Init under emulation can
    // legitimately take >20s (worst known cold: Super Guy ~12s incl. the sfBand probe), and
    // before this flag existed the 20s stall check measured from spawn — a slow init was
    // killed mid-init and respawn-looped forever, presenting as "never draws anything".
    private var awaitingFirstFrame = true
    private var watchdog: DispatchSourceTimer?
    private var restarting = false

    // addm 569: live control channel. The host's stdin is a pipe; on a settings
    // change we write "SET <idx> <val>\n" lines to it and the host applies them to
    // the SAME control-value slots the ADCTRL/ADCVSET env seeds (see poll_set in the
    // hosts). No process respawn — same PID, seamless frames. Respawn is kept only as
    // a fallback when the stdin pipe is unavailable (e.g. the process just died).
    private var stdinHandle: FileHandle?

    // Occlusion/hide pause: when set, the reader stops draining the pipe so pipe
    // backpressure blocks the host in write() at ~0% CPU. Cleared on resume; the
    // parser accumulates across reads so resuming mid-stream is seamless.
    private var paused = false

    // Bumped on every (re)spawn; a reader thread stops as soon as its generation
    // is stale, so a killed process's reader can't publish into a new one.
    private var generation = 0

    // addm 571: shared-memory transport state for the current spawn (written on
    // `queue` in _spawnProcess, read once by the reader, munmap+unlink'd in
    // _killProcess). shmPtr==nil => this spawn uses the stdout stream path.
    private var shmPtr: UnsafeMutableRawPointer?
    private var shmSize: Int = 0
    private var shmName: String?
    private var shmW = 0, shmH = 0
    // Whether the shm transport actually engaged (host acked 'F'); reported to
    // verification so it can assert the zero-copy path ran (vs a silent fallback).
    public private(set) var shmActive = false

    // Number of frames successfully decoded & delivered (for verification).
    // Written on `queue`; read after stop() (which drains `queue`).
    public private(set) var framesConsumed = 0

    // addm 569: frames dropped by the duplicate-frame suppressor (identical content
    // to the previously displayed frame -> decode + layer update skipped entirely).
    public private(set) var framesSkipped = 0

    // Current host PID (nil if not running) — for verification that a live SET does
    // NOT respawn the process.
    public var currentPID: Int32? { queue.sync { process?.processIdentifier } }

    public init(module: ADModule, settings: [String: Int],
                onFrame: @escaping (CGImage) -> Void) {
        self.module = module
        self.settings = settings
        self.onFrame = onFrame
    }

    deinit { stop() }

    // MARK: - Lifecycle
    public func start() {
        queue.async { [weak self] in self?._start() }
    }

    public func updateSettings(_ s: [String: Int]) {
        queue.async { [weak self] in
            guard let self else { return }
            let old = self.settings
            self.settings = s
            // addm 630: dll#/AD3-family 68K modules read their control values ONCE at
            // init (Modern Art proven: live SET lands in the slot but the module never
            // re-reads; an in-process re-init is not re-entrant). A POPUP change on a
            // 68K module therefore RESPAWNS with the new ADCVSET so init re-reads it —
            // this is what actually switches Modern Art's style. Sliders/checkboxes
            // keep the live-SET path (smooth, no restart; harmless if also init-read
            // since the value persists for any later respawn).
            let popupChanged = self.module.family == .k68 && self.module.controls.contains {
                if case .popup = $0.kind { return (old[$0.id] ?? $0.defaultValue) != (s[$0.id] ?? $0.defaultValue) }
                return false
            }
            // addm 569: otherwise control-value changes go over the live SET channel
            // (no respawn -> same PID, uninterrupted frames). Fall back to a respawn
            // only if the stdin pipe isn't there (process gone / never started).
            if !popupChanged, self.running, self.process?.isRunning == true, self.stdinHandle != nil {
                self._sendControlsLive()
            } else {
                self._respawnProcess()
            }
        }
    }

    public func stop() {
        queue.sync { [weak self] in self?._stop() }
    }

    // Pause/resume pipe consumption (called from occlusion / app-hide handlers).
    // Pausing idles the host via backpressure; resuming continues cleanly.
    public func setPaused(_ p: Bool) {
        queue.async { [weak self] in
            guard let self else { return }
            if self.paused != p {
                self.paused = p
                // Don't let the stall watchdog fire while intentionally paused.
                if !p { self.lastFrameAt = Date() }
            }
        }
    }

    // MARK: - Internals (all on `queue`)
    private func _start() {
        guard !running else { return }
        running = true
        _spawnProcess()
        // Lightweight stall watchdog: no frame for 20s => kill + respawn.
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1.0, repeating: 1.0)
        t.setEventHandler { [weak self] in self?._checkStall() }
        t.resume()
        watchdog = t
    }

    private func _stop() {
        running = false
        watchdog?.cancel(); watchdog = nil
        _killProcess()
    }

    private func _killProcess() {
        generation &+= 1          // orphan any live reader thread immediately
        stdinHandle = nil         // drop the live SET pipe with the process
        if let p = process, p.isRunning {
            p.terminationHandler = nil
            p.terminate()
            // Give it a beat, then hard-kill via signal if still alive.
            if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        }
        process = nil
        // addm 571: promptly remove the shm NAME (RAM is freed by the reader's
        // munmap on its exit — it owns the mapping). Clearing our copies means a
        // subsequent spawn can't accidentally re-clean this generation's slot.
        if let nm = shmName { shm_unlink(nm) }
        shmPtr = nil; shmName = nil; shmSize = 0; shmActive = false
    }

    // addm 569: push the current effective control values to the running host as
    // "SET <idx> <val>\n" lines. idx = resourceId-1000 (the same index the host's
    // ADCTRL/ADCVSET seeding uses); 68K clamps to 0..3, PPC to 0..15. Only controls
    // with a valid index are sent. Runs on `queue`.
    private func _sendControlsLive() {
        guard let handle = stdinHandle, let recipe = module.recipe else { return }
        let maxIdx = recipe.controlEnvVar == "ADCVSET" ? 4 : 16   // 68K: 4 slots, PPC: 16
        var lines = ""
        // FIRST-WINS per host index (matches _injectControls): two controls that
        // share a resource ID (Swirling Magic's mVal/xVal 1002) map to one slot, so
        // only the earlier one is sent — no self-conflicting SETs on the same index.
        var written = Set<Int>()
        for c in module.controls {
            let idx = c.resourceId - 1000
            guard idx >= 0, idx < maxIdx, !written.contains(idx) else { continue }
            written.insert(idx)
            lines += "SET \(idx) \(effective(c))\n"
        }
        guard !lines.isEmpty, let data = lines.data(using: .utf8) else { return }
        do { try handle.write(contentsOf: data) }
        catch {
            // Pipe write failed (host gone / full) -> fall back to a respawn so the
            // new settings still take effect.
            _respawnProcess()
        }
    }

    // MARK: - addm 574: live keyboard/mouse input channel
    // Forward real input to the running host over the SAME stdin pipe as SET/GO.
    // Protocol lines: "KEY <kc> <0|1>" (Mac virtual keycode down/up), "CAPS <0|1>"
    // (caps-lock STATE — macOS reports caps as a modifier state, not down/up),
    // "MOUSE <x> <y> <btn>" (frame-local coords). The host maintains a live Mac
    // KeyMap + caps + mouse state and serves it via GetKeys/Button/GetMouse, the
    // low-memory KeyMap (0x174), and GetOSEvent modifiers. Writes go ON `queue` so
    // they can't interleave with a concurrent GO/SET write to the same fd. No-op if
    // the host isn't running (stdin pipe gone) — input is best-effort, never fatal.
    public func sendKey(keycode: Int, down: Bool) { _sendInput("KEY \(keycode) \(down ? 1 : 0)\n") }
    // addm 797: retained as part of the addm-574 input protocol, but the app no
    // longer drives caps through it — the host reads the real key itself under ADREALCAPS
    // (see _buildEnv), which needs no focus and works in the screen-saver appex too.
    public func sendCaps(_ on: Bool) { _sendInput("CAPS \(on ? 1 : 0)\n") }
    public func sendMouse(x: Int, y: Int, button: Bool) { _sendInput("MOUSE \(x) \(y) \(button ? 1 : 0)\n") }
    private func _sendInput(_ line: String) {
        queue.async { [weak self] in
            guard let self, let h = self.stdinHandle else { return }
            try? h.write(contentsOf: Data(line.utf8))
        }
    }

    private func _respawnProcess() {
        guard running else { return }
        _killProcess()
        _spawnProcess()
    }

    private func _spawnProcess() {
        guard let recipe = module.recipe else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: recipe.host)
        p.currentDirectoryURL = URL(fileURLWithPath: recipe.workingDir)

        // Build argv from the template. {COPY} used to materialise a temp
        // data-fork copy for PPC; we now pass the original data-fork path
        // directly (read-only input to the host) — ZERO filesystem writes.
        var args: [String] = []
        for a in recipe.argsTemplate {
            switch a {
            case "{RSRC}":
                args.append(recipe.rsrcPath)
            case "{COPY}":
                args.append(recipe.dataForkPath ?? recipe.rsrcPath)
            default:
                args.append(a)
            }
        }
        p.arguments = args
        var env = _buildEnv(recipe)

        // addm 571: set up the zero-copy shm slot for THIS spawn. Geometry comes
        // from the same ADSCREENW/H the host reads (so the app-sized slot always
        // matches the host framebuffer). On any failure we simply don't pass
        // ADSHM and the host uses the P8/P6 stdout stream (env already has
        // ADSTREAM/ADSTREAMIDX), so this is strictly additive.
        shmActive = false
        if Self.shmEnabled {
            let w = Int(env["ADSCREENW"] ?? "") ?? 512
            let h = Int(env["ADSCREENH"] ?? "") ?? 384
            if let (ptr, name, size) = Self._createSHM(width: w, height: h) {
                shmPtr = ptr; shmSize = size; shmName = name; shmW = w; shmH = h
                env["ADSHM"] = "1"
                env["ADSHMNAME"] = name
            }
        }
        p.environment = env

        let pipe = Pipe()
        p.standardOutput = pipe
        // ADHOSTERR=1 (debug): surface the host's stderr instead of discarding it.
        p.standardError = ProcessInfo.processInfo.environment["ADHOSTERR"] != nil
            ? FileHandle.standardError : FileHandle.nullDevice
        // addm 569: give the host a stdin pipe for the live control channel. The
        // host polls it non-blocking each frame for "SET idx val" lines.
        let inPipe = Pipe()
        p.standardInput = inPipe

        p.terminationHandler = { [weak self] _ in
            guard let self else { return }
            self.queue.async {
                // Process exited (e.g. a finite saver ended). Respawn to keep
                // the live preview going, unless we're deliberately restarting.
                if self.running && !self.restarting {
                    self._killProcess()
                    self._spawnProcess()
                }
            }
        }
        do {
            try p.run()
            process = p
            stdinHandle = inPipe.fileHandleForWriting   // addm 569: live SET sink
            lastFrameAt = Date()
            awaitingFirstFrame = true
            generation &+= 1
            let gen = generation
            let handle = pipe.fileHandleForReading
            // Dedicated reader thread: blocking reads on the pipe, parse the P6
            // stream, publish frames. Exits at EOF (process death) or when its
            // generation goes stale.
            let thread = Thread { [weak self] in
                self?._readLoop(handle: handle, generation: gen)
            }
            thread.name = "adhost.reader.\(gen)"
            thread.stackSize = 1 << 20
            thread.start()
        } catch {
            process = nil
            // addm 571: no reader will run, so free the shm slot we just created.
            if let ptr = shmPtr { munmap(ptr, shmSize) }
            if let nm = shmName { shm_unlink(nm) }
            shmPtr = nil; shmName = nil; shmSize = 0
        }
    }

    private func _buildEnv(_ recipe: ADRecipe) -> [String: String] {
        // Minimal environment. The host reads ONLY AD*-prefixed variables (an audit
        // of every getenv in the host is 197 keys, all "AD"). Its PPC emulator debug
        // hook, however, evaluates ~30 of those getenv()s on EVERY emulated
        // instruction, and getenv is O(environment size) — a linear __findenv scan.
        // Inheriting the whole process environment (launchd/shell vars the host never
        // reads) therefore made every one of those millions of per-frame lookups scan
        // a long list; on an instruction-heavy module (Swirling Magic's 70-pixie
        // swirl) that alone was the frame-rate bottleneck. Passing only what the host
        // (and process/dyld startup) actually needs roughly halves the per-frame cost.
        // We still forward any AD*/DYLD_* lever a developer set in their shell, plus a
        // small harmless system base for process startup and the real-clock timezone.
        var env: [String: String] = [:]
        let inherited = ProcessInfo.processInfo.environment
        for (k, v) in inherited where k.hasPrefix("AD") || k.hasPrefix("DYLD_") { env[k] = v }
        for k in ["HOME", "PATH", "TMPDIR", "USER", "LOGNAME", "LANG", "TZ"] {
            if let v = inherited[k] { env[k] = v }
        }
        // Stream frames to stdout (no frame dir). ADSTREAMIDX requests the indexed
        // "P8" format (palette + 1 byte/pixel) which the reader decodes with an
        // indexed CGImage (no per-pixel work). A host that predates P8 simply emits
        // P6 and the reader auto-detects it per frame, so this is compat-safe.
        env["ADSTREAM"] = "1"
        // addm 797: let the host read the REAL Caps-Lock key itself, once per
        // frame. 19 catalog modules document a caps behaviour (Time Flies changes the clock
        // type, Magic Turtle opens its edit window, Mandelbrot/Satori/Psycho Deli/Swirling
        // Magic recolour); they poll it through the shared library's GetCapsLockChange(),
        // which reads the low-memory Mac KeyMap the host already maintains. The host was
        // always able to serve it — nothing was ever telling it what caps was doing.
        //
        // Host-side polling rather than an app->host input channel, because it needs no
        // focus, no first responder, and no event delivery — so it works identically in the
        // app pane and inside the screen-saver appex, and it survives host respawns and
        // module cycling for free. The host uses CGEventSourceFlagsState's alphaShift bit,
        // which is a modifier-state read: it triggers NO permission prompt (verified from a
        // fresh binary path — zero com.apple.TCC log entries; see capslock_evidence.md).
        // Default-off in the host, so headless/census runs stay byte-deterministic.
        env["ADREALCAPS"] = "1"
        // addm 801: seed the AD4 library's lagged-Fibonacci RNG per launch, the way a real
        // Mac did — modules seed themselves with SRand(GetMilliseconds()) and real
        // Microseconds() counted from BOOT (varied every launch); our virtual clock counts
        // from process start (a constant), which made every "Random" pick identical across
        // launches. App/saver only; headless and census paths never set this, so byte-exact
        // baselines are untouched.
        env["ADLIBRNGSEED"] = "random"
        env["ADSTREAMIDX"] = "1"
        // Static recipe env.
        for (k, v) in recipe.env { env[k] = v }
        // addm 744: arm the authentic Duration-cycle safety net on BOTH hosts.
        // ADAUTORESTART is dead in the 68K host as of addm 701 (see adhost68k.cc
        // ~line 12848: "ADAUTORESTART no longer arms a cycle"); the real lever is
        // ADCYCLE (adhost68k.cc ~line 12859, adhost.cc ~line 3354 — both hosts read
        // it). Without this, a module that faults to black (e.g. Flying Toasters,
        // P0-1 in the visual census) stays black forever in the app. Unconditional
        // for both viewer and saver, PPC and 68K.
        env["ADCYCLE"] = "1"
        // addm 796: ...and let the user set the PERIOD, the way the real
        // control panel did. The hosts read ADCYCLESECS (adhost.cc ~3478,
        // adhost68k.cc ~14328) and gate the whole cycle on ADNOCYCLE (adhost.cc ~3463,
        // adhost68k.cc ~14308), so "Forever!" is expressed by disarming the cycle
        // outright rather than by a huge period. Left alone (nil) the host keeps its
        // own 60 s, which IS the factory default (sVal 503), so this is a no-op for a
        // user who never touches the slider. The preference deliberately overrides an
        // inherited ADCYCLESECS/ADNOCYCLE the same way the ADCYCLE=1 above already
        // overrides the environment — where there is a UI, the UI is the truth.
        if let secs = Self.durationSeconds {
            if secs == ADDuration.forever {
                env.removeValue(forKey: "ADCYCLE")
                env.removeValue(forKey: "ADCYCLESECS")
                env["ADNOCYCLE"] = "1"
            } else {
                env.removeValue(forKey: "ADNOCYCLE")
                env["ADCYCLESECS"] = String(secs)
            }
        }
        // Family/lane defaults.
        if module.family == .k68 {
            env["ADTICKRATE"] = "120"
            env["ADMAXTRAPS"] = "4000000000"
            // No disk risk with streaming: pipe backpressure paces the host and
            // SIGPIPE reaps any orphan. Let finite savers run effectively forever;
            // the terminationHandler respawns any that self-exit.
            env["ADFRAMES"] = "900000"
            if recipe.lane == "l40" {
                env["ADSCREENW"] = "640"; env["ADSCREENH"] = "480"
                env["ADLINKMODULE"] = "1"; env["ADDLLMAXI"] = "0"; env["ADSECSRATE"] = "1"
            } else {
                env["ADSCREENW"] = "512"; env["ADSCREENH"] = "384"
            }
        } else {
            env["ADRSRC"] = recipe.rsrcPath
            env["ADLIB"] = "AD4Library.rsrc"
            env["ADFRAMES"] = "900000"
        }
        env.removeValue(forKey: "ADFRAMEDIR")   // ensure no file transport
        // Control values (only when the user has moved something off default;
        // Magic Turtle keeps its required ADCTRL from the recipe regardless).
        _injectControls(&env, recipe)
        return env
    }

    private func effective(_ c: ADControl) -> Int { settings[c.id] ?? c.defaultValue }
    private var anyChanged: Bool {
        module.controls.contains { effective($0) != $0.defaultValue }
    }

    private func _injectControls(_ env: inout [String: String], _ recipe: ADRecipe) {
        if recipe.controlEnvVar == "ADCVSET" {
            // 68K: sparse "idx:val,..." listing every control's effective value.
            guard anyChanged else { return }
            let parts = module.controls.compactMap { c -> String? in
                let idx = c.resourceId - 1000
                guard idx >= 0 else { return nil }
                return "\(idx):\(effective(c))"
            }
            if !parts.isEmpty {
                env["ADCVSET"] = parts.joined(separator: ",")
                env["ADCVDEFAULT"] = "12"
            }
        } else {
            // PPC: dense 16-short ADCTRL. The host seeds EVERY control-value slot to
            // 50 unless ADCTRL overrides it (adhost.cc: GetControlValue(idx) reads
            // (*(adxplTOC+0x3A8))[idx]). Unlike the 68K host — which reads each
            // module's sVal/mVal/xVal/bVal factory value straight from its resource
            // fork — the PPC host has no faceplate, so if we send nothing the module
            // runs at 50-for-everything, NOT its authentic 1990s factory defaults.
            // Therefore ALWAYS emit ADCTRL (seeded with the extracted factory
            // defaults, overlaid with any user change), so a module the user has
            // never touched still starts at its factory settings. Recipe ADCTRL
            // (Magic Turtle) remains the base when present.
            let hasRecipeCtrl = recipe.env["ADCTRL"] != nil
            let hasIndexable = module.controls.contains { $0.resourceId >= 1000 }
            guard hasRecipeCtrl || hasIndexable else { return }
            // Pad unbacked slots with 0, not 50: real After Dark's GetControlValue
            // returns 0 for indices beyond a module's control resources, and some
            // modules GATE features on that (Time Flies caps clock-swap reads slot 3
            // == 0; addm 623). The host now zero-clamps too; keep the two in accord.
            var seed = [Int](repeating: 0, count: 16)
            if let s = recipe.env["ADCTRL"] {
                let nums = s.split(separator: ",").compactMap { Int($0) }
                for (i, n) in nums.enumerated() where i < 16 { seed[i] = n }
            }
            // Overlay each control's effective value (user value or factory default)
            // at its host index = resourceId-1000. FIRST-WINS per index: when two
            // controls share a resource ID (Swirling Magic's mVal/xVal 1002 both map
            // to slot 2) the earlier control in catalog order keeps the slot, which
            // mirrors the 68K host's sVal→mVal→xVal→bVal first-wins tiling and keeps
            // the primary popup/slider authentic rather than an auxiliary toggle
            // clobbering it. (The second same-ID control's value can't reach a
            // distinct slot — an inherent limit of the shared resource ID.)
            var written = Set<Int>()
            for c in module.controls {
                let idx = c.resourceId - 1000
                guard idx >= 0, idx < 16, !written.contains(idx) else { continue }
                seed[idx] = effective(c)
                written.insert(idx)
            }
            env["ADCTRL"] = seed.map(String.init).joined(separator: ",")
        }
    }

    // MARK: - Stall watchdog (on `queue`)
    private func _checkStall() {
        guard running, !paused else { return }   // paused => intentional stall
        // 20s of silence BETWEEN frames means a wedged host; before the FIRST frame the
        // process is initializing, which is not a hang — allow 120s so no legitimate init
        // is ever executed mid-flight, while a truly hung init still recovers eventually.
        let limit: TimeInterval = awaitingFirstFrame ? 120 : 20
        if Date().timeIntervalSince(lastFrameAt) > limit {
            restarting = true
            _respawnProcess()
            restarting = false
            lastFrameAt = Date()
        }
    }

    // MARK: - Reader dispatch (dedicated thread)
    // addm 571: if a shm slot was set up for this spawn, drive the GO/F lockstep
    // over shared memory. The first ack byte confirms the shm path (host wrote
    // 'F'); anything else means an old/non-shm host is P8/P6-streaming on stdout,
    // so we fall back transparently, seeding the byte we already read.
    private func _readLoop(handle: FileHandle, generation gen: Int) {
        // Snapshot this spawn's shm mapping. The reader thread is the SOLE owner of
        // the munmap: it unmaps + unlinks on EVERY exit path (defer), so nothing
        // else ever frees a slot the reader might still be copying out of.
        // _killProcess only bumps the generation (stops us) + shm_unlink's the name
        // for prompt removal (harmless if we also unlink). munmap stays reader-only.
        var shm: (ptr: UnsafeMutableRawPointer, w: Int, h: Int, name: String, size: Int)?
        queue.sync {
            if self.generation == gen, let p = self.shmPtr, let nm = self.shmName {
                shm = (p, self.shmW, self.shmH, nm, self.shmSize)
            }
        }
        defer {
            if let shm { munmap(shm.ptr, shm.size); shm_unlink(shm.name) }
        }
        if let shm {
            let s = (ptr: shm.ptr, w: shm.w, h: shm.h)
            switch _shmProbe(handle: handle, generation: gen, shm: s) {
            case .confirmed:
                _shmLoop(handle: handle, generation: gen, shm: s)
                return
            case .fallback(let seed):
                _streamLoop(handle: handle, generation: gen, seed: seed)
                return
            case .dead:
                return
            }
        }
        _streamLoop(handle: handle, generation: gen, seed: nil)
    }

    // Outcome of the first GO/ack round-trip.
    private enum ShmProbe { case confirmed; case fallback(Data); case dead }

    // Send the first GO and read one stdout byte. 'F' => shm path; any other byte
    // => this host is streaming P8/P6 (old binary) — return it as a fallback seed.
    // Reads via poll() so stop()/supersede breaks out even if the byte never comes.
    private func _shmProbe(handle: FileHandle, generation gen: Int,
                           shm: (ptr: UnsafeMutableRawPointer, w: Int, h: Int)) -> ShmProbe {
        _sendGO()
        let fd = handle.fileDescriptor
        while true {
            let (alive, _) = state(gen)
            if !alive { return .dead }
            var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let pr = poll(&pfd, 1, 200)          // 200ms tick so we re-check liveness
            if pr < 0 { return .dead }
            if pr == 0 { continue }
            var byte: UInt8 = 0
            let r = read(fd, &byte, 1)
            if r <= 0 { return .dead }            // EOF (host died)
            if byte == 0x46 {                     // 'F'
                queue.async { [weak self] in guard let self, self.generation == gen else { return }; self.shmActive = true }
                return .confirmed
            }
            return .fallback(Data([byte]))        // old host: seed the stream parser
        }
    }

    private func _sendGO() {
        // Write "GO\n" to the host's stdin (the same pipe the live SET channel
        // uses). Done ON `queue` so it can't interleave with a concurrent
        // _sendControlsLive() SET write to the same fd. A broken pipe (host gone)
        // fails silently; the reader then hits EOF on stdout and exits.
        queue.sync {
            guard let h = self.stdinHandle else { return }
            try? h.write(contentsOf: Data("GO\n".utf8))
        }
    }

    // MARK: - SHM lockstep loop (dedicated thread)
    // Strict request-response: send GO, block for the 'F' ack, copy the palette+
    // index plane OUT of the mapped slot into an owned buffer (see the retain note
    // below), decode + dedupe + present, pace, repeat. Occlusion pause = stop
    // sending GO so the host blocks in read() at ~0% CPU.
    //
    // RETAIN-SEMANTICS DECISION (single slot + copy-out): the mapped slot is stable
    // only until the NEXT GO (lockstep guarantee). A zero-copy CGImage over the
    // mapped bytes would let CoreAnimation read the slot during async compositing —
    // potentially AFTER we send the next GO and the host overwrites it — with no
    // release signal we can safely gate GO on. Rather than escalate to a 2/3-slot
    // ring, we copy the ~w*h+768 bytes out of the slot into an owned Data before
    // the next GO. That copy REPLACES the pipe's two kernel copies (write+read) the
    // stdout path incurred, so it is still cheaper end-to-end, and makes single-slot
    // correct by construction — the CGImage never references shared memory, so no
    // tearing is possible. Measured: no visual difference vs the P8 path.
    private func _shmLoop(handle: FileHandle, generation gen: Int,
                          shm: (ptr: UnsafeMutableRawPointer, w: Int, h: Int)) {
        let fd = handle.fileDescriptor
        var costEMA = 0.020
        var lastBody: Data? = nil
        var first = true      // frame 0's 'F' was already consumed by the probe
        var lastPresent = Date()
        while true {
            let (alive, isPaused) = state(gen)
            if !alive { break }
            if isPaused { Thread.sleep(forTimeInterval: 0.05); continue }   // stop GO -> host idle

            if !first {
                _sendGO()
                // Block for the 'F' ack, but wake every 200ms to re-check state so
                // stop()/pause is responsive even if the host is mid-render.
                var acked = false
                while true {
                    let (a2, p2) = state(gen)
                    if !a2 { return }
                    if p2 { break }                 // paused mid-wait: drop this GO, loop resends
                    var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                    let pr = poll(&pfd, 1, 200)
                    if pr < 0 { return }
                    if pr == 0 { continue }
                    var byte: UInt8 = 0
                    let r = read(fd, &byte, 1)
                    if r <= 0 { return }            // host died
                    if byte == 0x46 { acked = true; break }
                }
                if !acked { continue }
            }
            first = false

            // Copy the frame OUT of the slot (correctness — see the note above).
            let n = 768 + shm.w * shm.h
            let body = Data(bytes: shm.ptr.advanced(by: 64), count: n)

            // Exact byte comparison, NOT a sampled hash: stride-sampled hashes alias
            // under small-delta animation (Snake: 92% of frames falsely skipped; the
            // Psycho Deli palette rotation was the same bug in the palette region).
            // A full memcmp of a ~300KB body is ~10us — cheaper than any decode it
            // could ever save incorrectly.
            let isDup = Self.dedupeEnabled && lastBody == body
            lastBody = body
            queue.async { [weak self] in
                guard let self, self.generation == gen else { return }
                self.awaitingFirstFrame = false
                self.lastFrameAt = Date()
                self.framesConsumed += 1
                if isDup { self.framesSkipped += 1 }
            }
            // Pace to the fps cap by sleeping only the REMAINDER of the target
            // interval since the last frame — a slow render (which already exceeds
            // the interval) is never further delayed, so lockstep throughput tracks
            // the host's real frame time instead of render+cap serialized.
            func pace(_ interval: Double) {
                let rem = interval - Date().timeIntervalSince(lastPresent)
                if rem > 0 { Thread.sleep(forTimeInterval: rem) }
                lastPresent = Date()
            }
            if isDup { pace(1.0 / 60.0); continue }

            let t0 = Date()
            // autoreleasepool: decodeP8 autoreleases CG/CF objects on this raw
            // Thread; without a per-frame drain they accumulate and the deferred
            // bulk release stalls the reader ~165ms every ~0.5s — the user-visible
            // periodic hitch (host ack latency is uniform; addm 613/618).
            autoreleasepool {
                if let img = Self.decodeP8(body, width: shm.w, height: shm.h) { onFrame(img) }
            }
            let cost = Date().timeIntervalSince(t0)
            costEMA = costEMA * 0.8 + cost * 0.2
            pace(costEMA < 0.008 ? 1.0 / 60.0 : 1.0 / 30.0)
        }
    }

    // MARK: - P6/P8 stream reader (dedicated thread) — fallback / shm-disabled path
    private func _streamLoop(handle: FileHandle, generation gen: Int, seed: Data?) {
        var buf = Data()
        buf.reserveCapacity(1 << 21)
        if let seed { buf.append(seed) }
        // Adaptive delivery cap: EMA of the per-frame decode+deliver cost. When it
        // stays cheap (<8ms) we read at 60fps, otherwise 30. The cap paces reads;
        // pipe backpressure then paces the host to match.
        var costEMA = 0.020
        // addm 569: duplicate-frame suppression state (last DISPLAYED frame's hash).
        var lastBody: Data? = nil
        while true {
            // Stop promptly if we've been superseded/stopped.
            let (alive, isPaused) = state(gen)
            if !alive { break }
            // Occluded/hidden: stop draining so the host blocks on write() at ~0%
            // CPU. Poll for resume without touching the pipe.
            if isPaused { Thread.sleep(forTimeInterval: 0.05); continue }

            let chunk = handle.availableData      // blocks; empty == EOF
            if chunk.isEmpty { break }
            buf.append(chunk)

            // Drain every complete frame currently buffered, but only decode &
            // publish the NEWEST (dropping intermediates). Under backpressure the
            // buffer normally holds a single frame; this just guards bursts.
            var latest: (Data, Int, Int, Int)?     // (body, w, h, tag)
            var consumed = 0
            while let f = Self.takeFrame(&buf) {
                latest = f
                consumed += 1
            }
            guard consumed > 0, let (body, w, h, tag) = latest else { continue }
            if !state(gen).alive { break }
            let n = consumed
            // addm 569: duplicate-frame suppression. A cheap FNV-1a hash over every
            // 64th body byte + length; if identical to the last frame we DISPLAYED,
            // skip decode + CALayer update entirely (a static/slow module produces
            // byte-identical frames). Hashing the full P8 body (palette + indices)
            // means a CLUT-only animation still changes the hash -> never falsely
            // skipped, so palette-animation savers keep animating.
            // Exact byte comparison (see the shm-loop note: sampled hashes alias).
            let isDup = Self.dedupeEnabled && lastBody == body
            lastBody = body
            queue.async { [weak self] in
                guard let self, self.generation == gen else { return }
                self.awaitingFirstFrame = false
                self.lastFrameAt = Date()
                self.framesConsumed += n
                if isDup { self.framesSkipped += 1 }
            }
            if isDup {
                // Nothing new to show; still pace so we don't busy-spin on a static
                // stream (pipe backpressure keeps the host idle too).
                Thread.sleep(forTimeInterval: 1.0 / 60.0)
                continue
            }
            let t0 = Date()
            let img = tag == 8 ? Self.decodeP8(body, width: w, height: h)
                               : Self.decodeRGB(body, width: w, height: h)
            if let img { onFrame(img) }
            let cost = Date().timeIntervalSince(t0)
            costEMA = costEMA * 0.8 + cost * 0.2
            // Adaptive cap: 60fps when the pipeline is cheap, else 30.
            let interval = costEMA < 0.008 ? 1.0 / 60.0 : 1.0 / 30.0
            Thread.sleep(forTimeInterval: interval)
        }
    }

    // Single queue round-trip returning liveness + pause state for the reader.
    private func state(_ gen: Int) -> (alive: Bool, paused: Bool) {
        var alive = false, p = false
        queue.sync { alive = self.running && self.generation == gen; p = self.paused }
        return (alive, p)
    }

    // MARK: - P6/P8 stream parsing
    // Removes the leading complete frame from `buf` and returns (body, w, h, tag)
    // where tag is 6 (RGB, w*h*3 body) or 8 (indexed: 768-byte palette + w*h
    // indices). Returns nil if a whole frame is not yet buffered (need more bytes)
    // — the buffer is left untouched in that case. Auto-detects the format per
    // frame so a P6-only host still works (P8 request ignored by old hosts).
    static func takeFrame(_ buf: inout Data) -> (Data, Int, Int, Int)? {
        guard let (bodyStart, w, h, tag) = parseHeader(buf) else { return nil }
        let bodyLen = tag == 8 ? (768 + w * h) : (w * h * 3)
        let frameEnd = bodyStart + bodyLen
        guard buf.count >= frameEnd else { return nil }
        let body = buf.subdata(in: bodyStart..<frameEnd)
        buf.removeSubrange(0..<frameEnd)
        return (body, w, h, tag)
    }

    // Parses a "P6" header (w, h, maxval) or our private "P8" indexed header
    // (w, h — no maxval), tolerating whitespace and '#' comments. Returns
    // (bodyStart, w, h, tag). bodyStart is the first body byte (one whitespace
    // after the last numeric token). nil if the header isn't fully present yet.
    static func parseHeader(_ b: Data) -> (Int, Int, Int, Int)? {
        let n = b.count
        var i = 0
        func skipWS() -> Bool {
            while i < n {
                let c = b[i]
                if c == 0x23 {                        // '#': comment to end of line
                    while i < n && b[i] != 0x0A { i += 1 }
                } else if c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D {
                    i += 1
                } else { return true }
            }
            return false                              // ran out mid-whitespace
        }
        // Read a token; returns nil if the buffer ends before a delimiter (so we
        // can't be sure the token is complete).
        func token() -> String? {
            guard skipWS() else { return nil }
            let start = i
            while i < n {
                let c = b[i]
                if c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x23 { break }
                i += 1
            }
            if i >= n { return nil }                  // no trailing delimiter yet
            return String(bytes: b[start..<i], encoding: .ascii)
        }
        guard let magic = token(), magic == "P6" || magic == "P8" else { return nil }
        let tag = magic == "P8" ? 8 : 6
        guard let ws = token(), let w = Int(ws), w > 0, w < 20000,
              let hs = token(), let h = Int(hs), h > 0, h < 20000
        else { return nil }
        if tag == 6 {
            // P6 has a maxval token before the body.
            guard let ms = token(), let maxv = Int(ms), maxv > 0, maxv < 256
            else { return nil }
        }
        // Exactly one whitespace byte separates the last token from the body.
        // `token()` stopped at that delimiter (i < n guaranteed above).
        return (i + 1, w, h, tag)
    }

    // addm 569: cheap 64-bit FNV-1a over every 64th body byte + length. Used by the
    // reader's duplicate-frame suppressor to detect a byte-identical frame without a
    // full compare. Sampling 1/64 of the plane keeps it ~free while still catching any
    // real motion (an unchanged frame samples identically; a changed one almost never
    // collides at 64 bits).

    // addm 569: palette/colorspace cache. Building an indexed CGColorSpace from the
    // 768-byte CLUT every frame is pure waste when the palette is unchanged (the common
    // case — only CLUT-animation savers rotate it). Cache the last palette + its indexed
    // colour space and reuse until the palette CONTENT actually changes (Data == is a
    // memcmp), so CLUT-animation savers (Zooommm!, Psycho Deli) still rebuild on every
    // real palette change and keep animating their colours.
    private static let csLock = NSLock()
    private static var csPalette: Data?
    private static var csSpace: CGColorSpace?
    private static let csBase = CGColorSpace(name: CGColorSpace.sRGB)

    static func indexedColorSpace(forPalette pal: Data) -> CGColorSpace? {
        csLock.lock(); defer { csLock.unlock() }
        if let cached = csSpace, let cp = csPalette, cp == pal { return cached }
        guard let base = csBase else { return nil }
        let space: CGColorSpace? = pal.withUnsafeBytes { raw in
            CGColorSpace(indexedBaseSpace: base, last: 255,
                         colorTable: raw.bindMemory(to: UInt8.self).baseAddress!)
        }
        csPalette = pal
        csSpace = space
        return space
    }

    // Decodes a P8 body (768-byte RGB palette + w*h index bytes) to an indexed
    // CGImage over the raw index plane — NO per-pixel Swift work. CoreGraphics
    // resolves colours through the indexed colour space at draw time.
    public static func decodeP8(_ body: Data, width w: Int, height h: Int) -> CGImage? {
        guard body.count >= 768 + w * h else { return nil }
        let indices = body.subdata(in: 768..<(768 + w * h))
        guard let cs = indexedColorSpace(forPalette: body.subdata(in: 0..<768)) else { return nil }
        guard let provider = CGDataProvider(data: indices as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 8,
                       bytesPerRow: w, space: cs, bitmapInfo: CGBitmapInfo(rawValue: 0),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }

    // Decodes a P6 body (w*h*3 RGB bytes) to a CGImage (RGB, opaque, no alpha).
    // Public so adrender's --verify-letterbox can build test frames the same way.
    public static func decodeRGB(_ body: Data, width w: Int, height h: Int) -> CGImage? {
        let needed = w * h * 3
        guard body.count >= needed else { return nil }
        var rgba = [UInt8](repeating: 255, count: w * h * 4)
        body.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let src = raw.bindMemory(to: UInt8.self)
            var s = 0, d = 0
            for _ in 0..<(w * h) {
                rgba[d]     = src[s]
                rgba[d + 1] = src[s + 1]
                rgba[d + 2] = src[s + 2]
                s += 3; d += 4
            }
        }
        let cs = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(data: &rgba, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
            return nil
        }
        return ctx.makeImage()
    }

    // MARK: - Shared-memory slot (addm 571)
    // Monotonic suffix so each spawn's shm name is unique (macOS shm names are
    // capped at ~31 chars, so keep it short: "/ad<pid>_<n>").
    private static let shmCounterLock = NSLock()
    private static var shmCounter: UInt64 = 0
    private static func nextShmSuffix() -> UInt64 {
        shmCounterLock.lock(); defer { shmCounterLock.unlock() }
        shmCounter &+= 1; return shmCounter
    }

    // shm_open is variadic in the SDK header (unavailable to Swift), so resolve it
    // as a fixed 3-arg function pointer via RTLD_DEFAULT.
    private typealias ShmOpenFn = @convention(c) (UnsafePointer<CChar>?, Int32, mode_t) -> Int32
    private static let shmOpenFn: ShmOpenFn? = {
        let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: -2)
        guard let sym = dlsym(RTLD_DEFAULT, "shm_open") else { return nil }
        return unsafeBitCast(sym, to: ShmOpenFn.self)
    }()

    // Create + size + map a POSIX shm object [64B header + 768B palette + w*h
    // indices]. Returns (mappedPtr, name, size) or nil on any failure (caller then
    // falls back to the stdout stream). The header is written by the host; we just
    // zero the region so a torn first read can't show garbage.
    static func _createSHM(width w: Int, height h: Int) -> (UnsafeMutableRawPointer, String, Int)? {
        guard w > 0, h > 0 else { return nil }
        let size = 64 + 768 + w * h
        let name = "/ad\(getpid())_\(nextShmSuffix())"
        shm_unlink(name)                                   // clear any stale object
        // adshm_open_create (C shim): shm_open is VARIADIC — calling it through a
        // fixed-signature Swift pointer silently DROPS the mode on arm64 (variadic
        // args live on the stack), creating the object with garbage permissions;
        // the host's attach then failed EACCES and every module silently ran the
        // stdout-stream fallback (whose burst-drain caused the periodic hitch).
        let fd = name.withCString { adshm_open_create($0, 0o600) }
        if fd < 0 { return nil }
        if ftruncate(fd, off_t(size)) != 0 { close(fd); shm_unlink(name); return nil }
        let p = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        close(fd)
        guard let ptr = p, ptr != UnsafeMutableRawPointer(bitPattern: -1) else {
            shm_unlink(name); return nil
        }
        memset(ptr, 0, size)
        return (ptr, name, size)
    }

    // MARK: - Headless smoke test (for adrender verification)
    // Runs the pipe pipeline for `seconds`, returns frames consumed. Cleans up
    // fully (guaranteed terminate).
    public static func smokeTest(module: ADModule, seconds: Double) -> Int {
        let sem = DispatchSemaphore(value: 0)
        let host = EmulatedHost(module: module, settings: [:]) { _ in }
        host.start()
        DispatchQueue.global().asyncAfter(deadline: .now() + seconds) { sem.signal() }
        sem.wait()
        let n = host.framesConsumed
        host.stop()
        return n
    }

    // addm 571: shm-transport smoke. Runs the pipeline for `seconds` and reports
    // frames consumed, whether the zero-copy shm path actually engaged (host acked
    // 'F'), and the PID (stable => no respawn). Set `shm` false to force the P8
    // stdout fallback (leaves the global toggle as the caller found it).
    public struct ShmSmoke { public let consumed: Int; public let shmActive: Bool; public let pid: Int32? }
    public static func smokeShmTest(module: ADModule, seconds: Double, shm: Bool) -> ShmSmoke {
        let prev = EmulatedHost.shmEnabled
        EmulatedHost.shmEnabled = shm
        defer { EmulatedHost.shmEnabled = prev }
        let host = EmulatedHost(module: module, settings: [:]) { _ in }
        host.start()
        Thread.sleep(forTimeInterval: seconds)
        let c = host.framesConsumed
        let active = host.shmActive
        let pid = host.currentPID
        host.stop()
        return ShmSmoke(consumed: c, shmActive: active, pid: pid)
    }

    // addm 569: run the pipeline for `seconds` and report frames produced + frames
    // dropped by the duplicate-frame suppressor (skip rate = skipped/consumed).
    public static func smokeMeasure(module: ADModule, seconds: Double) -> (consumed: Int, skipped: Int) {
        let host = EmulatedHost(module: module, settings: [:]) { _ in }
        host.start()
        Thread.sleep(forTimeInterval: seconds)
        let c = host.framesConsumed, s = host.framesSkipped
        host.stop()
        return (c, s)
    }

    // addm 569: live-control smoke. Runs the host for `seconds`, sends a control SET
    // at the halfway point, and reports frames before/after, the PID before/after (to
    // prove NO respawn), and the dedup skip count. Used by adrender --verify-emulation.
    public struct ControlSmoke {
        public let before: Int
        public let after: Int
        public let skipped: Int
        public let pidStable: Bool
    }
    public static func smokeControlTest(module: ADModule, seconds: Double) -> ControlSmoke {
        let host = EmulatedHost(module: module, settings: [:]) { _ in }
        host.start()
        Thread.sleep(forTimeInterval: seconds / 2)
        let pid0 = host.currentPID
        let before = host.framesConsumed
        // Move the first indexable control to a clearly different value.
        var s: [String: Int] = [:]
        if let c = module.controls.first(where: { $0.resourceId >= 1000 }) {
            let alt = c.defaultValue == 0 ? 1 : max(0, c.defaultValue / 2 + 1)
            s[c.id] = alt == c.defaultValue ? alt + 1 : alt
        }
        host.updateSettings(s)
        Thread.sleep(forTimeInterval: seconds / 2)
        let pid1 = host.currentPID
        let after = host.framesConsumed
        let skipped = host.framesSkipped
        host.stop()
        return ControlSmoke(before: before, after: after - before, skipped: skipped,
                            pidStable: pid0 != nil && pid0 == pid1)
    }

    // addm 702: the MIRROR of smokeControlTest for 68K popups. A 68K module reads its
    // control values once at init (addm 630), so a popup change must RESPAWN the host —
    // the opposite assertion from the live-SET path: the PID must CHANGE and frames must
    // keep coming. Modern Art's Style popup is the canonical case (field report: "only
    // Mondrian works"), so this pins the app half of that path: popup change -> respawn
    // -> the new value actually rides in ADCVSET on the new process.
    public struct PopupSmoke {
        public let before: Int
        public let after: Int
        public let respawned: Bool
        public let controlEnv: String?   // the ADCVSET the respawn would carry
    }
    public static func smokePopupTest(module: ADModule, seconds: Double) -> PopupSmoke {
        let host = EmulatedHost(module: module, settings: [:]) { _ in }
        host.start()
        Thread.sleep(forTimeInterval: seconds / 2)
        let pid0 = host.currentPID
        let before = host.framesConsumed
        var s: [String: Int] = [:]
        if let c = module.controls.first(where: {
            if case .popup = $0.kind { return $0.resourceId >= 1000 }; return false
        }) {
            s[c.id] = c.defaultValue == 1 ? 2 : 1
        }
        host.updateSettings(s)
        Thread.sleep(forTimeInterval: seconds / 2)
        let pid1 = host.currentPID
        let after = host.framesConsumed
        let env = host.controlEnvForCurrentSettings
        host.stop()
        return PopupSmoke(before: before, after: after - before,
                          respawned: pid0 != nil && pid1 != nil && pid0 != pid1,
                          controlEnv: env)
    }

    // The control env var (ADCVSET/ADCTRL) the next spawn would carry for the current
    // settings — lets a smoke assert the chosen value actually reaches the host.
    // addm 796: the cycle levers the NEXT spawn would carry, straight out of
    // the real _buildEnv — so a check can assert the Duration preference actually
    // becomes host environment rather than assuming the plumbing works. Empty ADCYCLESECS
    // + ADCYCLE=1 is the untouched-preference (host-default 60 s) state.
    public var cycleEnvForCurrentPreference: [String: String] {
        queue.sync {
            guard let r = module.recipe else { return [:] }
            let e = _buildEnv(r)
            return e.filter { ["ADCYCLE", "ADCYCLESECS", "ADNOCYCLE"].contains($0.key) }
        }
    }

    var controlEnvForCurrentSettings: String? {
        queue.sync {
            guard let r = module.recipe else { return nil }
            var e: [String: String] = [:]
            _injectControls(&e, r)
            return e[r.controlEnvVar]
        }
    }
}
