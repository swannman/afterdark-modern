import Foundation
import SpriteKit
import SwiftUI
import Metal
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import AfterDarkKit

// Offscreen renderer: render a module's scene to PNG frames without a window.
// usage: adrender <moduleId> <outPrefix> <width> <height> <t0,t1,t2,...seconds> [ctl=value ...]
// Trailing ctl=value args set module control values (the settings-inspector
// controls, e.g. count=4 color=0 toast=1) so variants can be rendered headless.

let args = CommandLine.arguments
let moduleId = args.count > 1 ? args[1] : "flying-toasters"
let outPrefix = args.count > 2 ? args[2] : "/tmp/adframe"
let W = args.count > 3 ? Int(args[3]) ?? 960 : 960
let H = args.count > 4 ? Int(args[4]) ?? 600 : 600
let times: [Double] = (args.count > 5 ? args[5] : "2,3,4")
    .split(separator: ",").compactMap { Double($0) }
var ctlSettings: [String: Int] = [:]
for a in args.dropFirst(6) {
    let kv = a.split(separator: "=", maxSplits: 1)
    if kv.count == 2, let v = Int(kv[1]) { ctlSettings[String(kv[0])] = v }
}

// --snapshot-inspector <outPath>: render the settings inspector panel itself
// (pure SwiftUI, headless via ImageRenderer) for visual verification.
if moduleId == "--snapshot-inspector" {
    MainActor.assumeIsolated {
        let out = args.count > 2 ? args[2] : "/tmp/inspector.png"
        let which = args.count > 3 ? args[3] : "flying-toasters"
        let mod = AD_MODULES.first { $0.id == which } ?? AD_MODULES.first { $0.id == "flying-toasters" }!
        let store = ADSettingsStore()
        let view = ModuleInspectorContent(module: mod, settings: store)
            .frame(width: 260)
            .background(Color(nsColor: .windowBackgroundColor))
            .environment(\.colorScheme, .dark)
        let renderer = ImageRenderer(content: view)
        renderer.scale = 2
        guard let img = renderer.cgImage else {
            FileHandle.standardError.write("inspector render failed\n".data(using: .utf8)!); exit(1)
        }
        let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: out) as CFURL,
                                                   UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(dest, img, nil)
        CGImageDestinationFinalize(dest)
        print("wrote \(out)")
        exit(0)
    }
}

// --smoke <moduleId> [seconds]: drive ONE module by id through the full app
// resolution path (EmulatedHost -> ADCatalog -> ADPaths) and report frames +
// the resolved host/workingDir. Useful for verifying the distributed run-path
// bridge with an arbitrary module (the fixed-module --verify-emulation can't).
if moduleId == "--smoke" {
    let id = args.count > 2 ? args[2] : "68k-bogglins"
    let secs = args.count > 3 ? (Double(args[3]) ?? 6.0) : 6.0
    print("ADPaths.isDistributed=\(ADPaths.isDistributed)")
    print("ADPaths.hostsDir=\(ADPaths.hostsDir)")
    print("ADPaths.sharedLibsRoot=\(ADPaths.sharedLibsRoot)")
    guard let m = AD_MODULES.first(where: { $0.id == id }) else {
        print("\(id): NOT IN CATALOG"); exit(1)
    }
    if let r = m.recipe {
        print("recipe.host=\(r.host)")
        print("recipe.workingDir=\(r.workingDir)")
        print("host exists=\(FileManager.default.isExecutableFile(atPath: r.host))")
    }
    let n = EmulatedHost.smokeTest(module: m, seconds: secs)
    print("\(id) [\(m.family.rawValue)]: framesConsumed=\(n) \(n > 0 ? "OK" : "FAIL")")
    exit(n > 0 ? 0 : 1)
}

// --verify-letterbox: headless check that LetterboxedFrameView presents a
// streamed frame aspect-fit, centered, with BLACK letterbox bars (never the
// window background). Renders an all-white 640x480 test frame into panes wider
// and taller than 4:3 and samples bar / frame pixels.
if moduleId == "--verify-letterbox" {
    MainActor.assumeIsolated {
        // Build an all-white opaque test frame the same way EmulatedHost does.
        let fw = 640, fh = 480
        let body = Data([UInt8](repeating: 255, count: fw * fh * 3))
        guard let frame = EmulatedHost.decodeRGB(body, width: fw, height: fh) else {
            print("=== verify-letterbox: FAIL (decodeRGB) ==="); exit(1)
        }
        func pixels(_ img: CGImage) -> (w: Int, h: Int, buf: [UInt8]) {
            let w = img.width, h = img.height
            var buf = [UInt8](repeating: 0, count: w * h * 4)
            let cs = CGColorSpaceCreateDeviceRGB()
            let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8,
                                bytesPerRow: w * 4, space: cs,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return (w, h, buf)
        }
        func rgb(_ p: (w: Int, h: Int, buf: [UInt8]), _ x: Int, _ y: Int) -> (UInt8, UInt8, UInt8) {
            let i = (y * p.w + x) * 4
            return (p.buf[i], p.buf[i + 1], p.buf[i + 2])
        }
        var pass = true
        @MainActor
        func check(_ name: String, pane: CGSize, blackAt: [(Int, Int)], whiteAt: [(Int, Int)]) {
            let view = LetterboxedFrameView(image: frame)
                .frame(width: pane.width, height: pane.height)
            let r = ImageRenderer(content: view)
            r.scale = 1
            guard let img = r.cgImage else { print("\(name): render FAIL"); pass = false; return }
            let p = pixels(img)
            var ok = true
            for (x, y) in blackAt {
                let c = rgb(p, x, y)
                if c != (0, 0, 0) { ok = false; print("  \(name) (\(x),\(y)) expected black got \(c)") }
            }
            for (x, y) in whiteAt {
                let c = rgb(p, x, y)
                if c != (255, 255, 255) { ok = false; print("  \(name) (\(x),\(y)) expected white got \(c)") }
            }
            print("\(name): pane=\(Int(pane.width))x\(Int(pane.height)) \(ok ? "OK" : "FAIL")")
            pass = pass && ok
        }
        // Wide pane (1000x500): fitted frame is 666x500 centered => side bars.
        check("wide-pane ", pane: CGSize(width: 1000, height: 500),
              blackAt: [(2, 250), (997, 250), (2, 2), (997, 497)],
              whiteAt: [(500, 250), (500, 2), (500, 497)])
        // Tall pane (500x800): fitted frame is 500x375 centered => top/bottom bars.
        check("tall-pane ", pane: CGSize(width: 500, height: 800),
              blackAt: [(250, 2), (250, 797), (2, 2), (497, 797)],
              whiteAt: [(250, 400), (2, 400), (497, 400)])
        print("=== verify-letterbox: \(pass ? "PASS" : "FAIL") ===")
        exit(pass ? 0 : 1)
    }
}

// --verify-emulation: headless smoke of the EmulatedHost pipeline (stdout P6
// pipe streaming). Spawns the real hosts for a couple of modules for ~5s each
// and reports frames consumed, then asserts: no host processes leaked, and ZERO
// filesystem artifacts (no adhost-* temp dirs, no frame*.ppm under the temp dir).
if moduleId == "--verify-emulation" {
    // Count only the actual host executables (exact names), not shell commands
    // whose arguments happen to mention the adhost directory.
    func pgrepHosts() -> [String] {
        var out: [String] = []
        for name in ["adhost", "adhost68k"] {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
            p.arguments = ["-x", name]
            let pipe = Pipe(); p.standardOutput = pipe
            try? p.run(); p.waitUntilExit()
            let d = pipe.fileHandleForReading.readDataToEndOfFile()
            let pids = String(data: d, encoding: .utf8)?
                .split(separator: "\n").map(String.init).filter { !$0.isEmpty } ?? []
            out.append(contentsOf: pids.map { "\(name):\($0)" })
        }
        return out
    }
    func freeGB() -> Double {
        let a = try? FileManager.default.attributesOfFileSystem(forPath: NSTemporaryDirectory())
        let free = (a?[.systemFreeSize] as? NSNumber)?.doubleValue ?? 0
        return free / 1_000_000_000
    }
    // Scan the temp dir for any file-transport leftovers.
    func scanArtifacts() -> (dirs: [String], ppms: [String]) {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        var dirs: [String] = [], ppms: [String] = []
        let en = FileManager.default.enumerator(at: tmp,
            includingPropertiesForKeys: [.isDirectoryKey], options: [])
        while let u = en?.nextObject() as? URL {
            let name = u.lastPathComponent
            if name.hasPrefix("adhost-") { dirs.append(u.path) }
            if name.hasPrefix("frame"), u.pathExtension == "ppm" { ppms.append(u.path) }
        }
        return (dirs, ppms)
    }

    let before = pgrepHosts().count
    let artBefore = scanArtifacts()
    var allFramesOK = true
    let targets = ["68k-bogglins", "ppc-fish-world"]
    for id in targets {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else {
            print("\(id): NOT IN CATALOG"); allFramesOK = false; continue
        }
        let n = EmulatedHost.smokeTest(module: m, seconds: 5.0)
        let ok = n > 0
        allFramesOK = allFramesOK && ok
        print("\(id) [\(m.family.rawValue)]: framesConsumed=\(n) \(ok ? "OK" : "FAIL")")
    }
    // Zero-copy shm transport smoke. With shm ON the host must ack 'F'
    // (shmActive) and deliver frames; with shm forced OFF the P8/P6 stdout fallback
    // must still deliver frames. Both run through the SAME EmulatedHost path.
    print("=== zero-copy shm transport (ADSHM) vs P8 stdout fallback ===")
    var shmOK = true
    for id in targets {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else { continue }
        let on = EmulatedHost.smokeShmTest(module: m, seconds: 5.0, shm: true)
        let off = EmulatedHost.smokeShmTest(module: m, seconds: 5.0, shm: false)
        let ok = on.shmActive && on.consumed > 0 && off.consumed > 0
        shmOK = shmOK && ok
        print("\(id) [\(m.family.rawValue)]: shm-on frames=\(on.consumed) shmActive=\(on.shmActive)"
            + "  p8-fallback frames=\(off.consumed) \(ok ? "OK" : "FAIL")")
    }

    // Live control-channel smoke. Send a control SET mid-stream and assert
    // the host keeps producing frames WITHOUT a respawn (PID unchanged) and no leak.
    // (Runs in the default shm-on mode, so it also exercises SET interleaved with GO.)
    print("=== live control channel (SET mid-stream) ===")
    var controlOK = true
    for id in targets {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else { continue }
        let r = EmulatedHost.smokeControlTest(module: m, seconds: 6.0)
        let ok = r.after > 0 && r.pidStable
        controlOK = controlOK && ok
        print("\(id) [\(m.family.rawValue)]: pre=\(r.before) post-SET=\(r.after) skipped=\(r.skipped) "
            + "pidStable=\(r.pidStable) \(ok ? "OK" : "FAIL")")
    }

    // 68K popup smoke — the mirror assertion. A 68K module reads its controls
    // once at init, so a popup change MUST respawn (new PID) and the chosen value must
    // ride in ADCVSET on the new process. Modern Art's Style popup is the canonical
    // case; this proves the app half of that path.
    print("=== 68K popup respawn (init-only controls) ===")
    var popupOK = true
    for id in ["68k-modern-art"] {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else { continue }
        let r = EmulatedHost.smokePopupTest(module: m, seconds: 6.0)
        let carries = r.controlEnv?.contains("0:") ?? false
        let ok = r.after > 0 && r.respawned && carries
        popupOK = popupOK && ok
        print("\(id) [\(m.family.rawValue)]: pre=\(r.before) post-popup=\(r.after) "
            + "respawned=\(r.respawned) ADCVSET=\(r.controlEnv ?? "nil") \(ok ? "OK" : "FAIL")")
    }

    // Give any terminating processes a moment, then check for leaks.
    Thread.sleep(forTimeInterval: 1.5)
    let leaked = pgrepHosts()
    print("host processes before=\(before) leaked-after=\(leaked.count) \(leaked.isEmpty ? "OK" : "FAIL")")
    for l in leaked { print("  LEAK: \(l)") }

    // Filesystem: only count artifacts that appeared during this run.
    let artAfter = scanArtifacts()
    let newDirs = artAfter.dirs.filter { !artBefore.dirs.contains($0) }
    let newPPMs = artAfter.ppms.filter { !artBefore.ppms.contains($0) }
    let filesOK = newDirs.isEmpty && newPPMs.isEmpty
    print("filesystem artifacts: adhost-dirs=\(newDirs.count) frame*.ppm=\(newPPMs.count) \(filesOK ? "OK (zero writes)" : "FAIL")")
    for d in newDirs { print("  DIR:  \(d)") }
    for f in newPPMs.prefix(10) { print("  PPM:  \(f)") }

    print(String(format: "disk free = %.1f GB (need >50)", freeGB()))
    // Print raw `df` for the temp volume.
    let df = Process()
    df.executableURL = URL(fileURLWithPath: "/bin/df")
    df.arguments = ["-h", NSTemporaryDirectory()]
    try? df.run(); df.waitUntilExit()

    let pass = allFramesOK && shmOK && controlOK && popupOK && leaked.isEmpty && filesOK
    print("=== verify-emulation: \(pass ? "PASS" : "FAIL") ===")
    exit(pass ? 0 : 1)
}

// --verify-dedupe [seconds] [moduleId ...]: measure the app-side CPU the
// --cadence <moduleId> [seconds]: timestamp every DISPLAYED onFrame delivery and
// report the inter-frame gap profile — used to hunt periodic hitches in the app
// delivery path (host-side cadence is measured separately by shmdrive613.c).
// Prints: fps, gap mean/median/p99/max, every gap > 2x median with its position
// on the timeline, and the deltas BETWEEN successive big gaps (a ~1.0s rhythm
// there = periodic hitch; random = noise).
if moduleId == "--cadence" {
    let id = args.count > 2 ? args[2] : "ppc-psycho-deli"
    let secs = args.count > 3 ? (Double(args[3]) ?? 20.0) : 20.0
    guard let m = AD_MODULES.first(where: { $0.id == id }) else {
        print("\(id): NOT IN CATALOG"); exit(1)
    }
    let lock = NSLock()
    var stamps: [Double] = []
    let t0 = Date()
    let host = EmulatedHost(module: m, settings: [:]) { _ in
        lock.lock(); stamps.append(Date().timeIntervalSince(t0)); lock.unlock()
    }
    host.start()
    Thread.sleep(forTimeInterval: secs)
    host.stop()
    lock.lock(); let ts = stamps; lock.unlock()
    guard ts.count > 5 else { print("only \(ts.count) frames — too few"); exit(1) }
    var gaps: [Double] = []
    for i in 1..<ts.count { gaps.append(ts[i] - ts[i-1]) }
    let sorted = gaps.sorted()
    let med = sorted[sorted.count/2], p99 = sorted[Int(Double(sorted.count)*0.99)]
    let mean = gaps.reduce(0,+)/Double(gaps.count)
    print(String(format: "%@: frames=%d over %.1fs (%.1f fps)  gap mean=%.1fms med=%.1fms p99=%.1fms max=%.1fms",
                 id, ts.count, secs, Double(ts.count)/secs, mean*1000, med*1000, p99*1000, sorted.last!*1000))
    var bigAt: [Double] = []
    for i in 1..<ts.count where (ts[i]-ts[i-1]) > med*2 { bigAt.append(ts[i]) }
    print("big gaps (>2x median): \(bigAt.count)")
    for (i,t) in bigAt.enumerated() {
        let d = i > 0 ? t - bigAt[i-1] : 0
        print(String(format: "  at t=%6.2fs  gap=%.1fms  delta-from-prev-big=%.2fs",
                     t, (gaps[ts.firstIndex(of: t)! - 1])*1000, d))
        if i > 24 { print("  ..."); break }
    }
    exit(0)
}

// duplicate-frame suppressor saves and the skip rate, on slow/static modules. Runs
// each module twice for `seconds` — dedupe OFF then ON — and reports getrusage CPU
// (this process only; the host is a separate process) plus skipped/consumed.
if moduleId == "--verify-dedupe" {
    let secs = args.count > 2 ? (Double(args[2]) ?? 6.0) : 6.0
    let mods = args.count > 3 ? Array(args.dropFirst(3))
                              : ["68k-super-guy", "68k-rose", "ppc-fish-world"]
    func cpuSeconds() -> Double {
        var ru = rusage()
        getrusage(RUSAGE_SELF, &ru)
        func tv(_ t: timeval) -> Double { Double(t.tv_sec) + Double(t.tv_usec) / 1_000_000 }
        return tv(ru.ru_utime) + tv(ru.ru_stime)
    }
    print("=== dedupe A/B (each module \(Int(secs))s OFF then ON) ===")
    for id in mods {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else {
            print("\(id): NOT IN CATALOG"); continue
        }
        EmulatedHost.dedupeEnabled = false
        let c0 = cpuSeconds()
        let off = EmulatedHost.smokeMeasure(module: m, seconds: secs)
        let cpuOff = cpuSeconds() - c0

        EmulatedHost.dedupeEnabled = true
        let c1 = cpuSeconds()
        let on = EmulatedHost.smokeMeasure(module: m, seconds: secs)
        let cpuOn = cpuSeconds() - c1

        let skipRate = on.consumed > 0 ? Double(on.skipped) / Double(on.consumed) * 100 : 0
        let saved = cpuOff > 0 ? (cpuOff - cpuOn) / cpuOff * 100 : 0
        print(String(format: "%-16@ produced=%d skipped=%d (%.0f%% skip)  app-CPU off=%.2fs on=%.2fs  saved=%.0f%%",
                     id as NSString, on.consumed, on.skipped, skipRate, cpuOff, cpuOn, saved))
    }
    EmulatedHost.dedupeEnabled = true
    exit(0)
}

// --measure-shm [seconds] [moduleId ...]: app-side A/B of the zero-copy
// shm transport vs the P8 stdout stream. For each module runs `seconds` with shm
// ON then OFF, reporting this-process (consumer) CPU via getrusage and achieved
// fps (framesConsumed/seconds). Host CPU is measured separately by the C harness.
if moduleId == "--measure-shm" {
    let secs = args.count > 2 ? (Double(args[2]) ?? 6.0) : 6.0
    let mods = args.count > 3 ? Array(args.dropFirst(3))
                              : ["68k-bogglins", "ppc-fish-world", "68k-fish-world"]
    func cpuSeconds() -> Double {
        var ru = rusage(); getrusage(RUSAGE_SELF, &ru)
        func tv(_ t: timeval) -> Double { Double(t.tv_sec) + Double(t.tv_usec) / 1_000_000 }
        return tv(ru.ru_utime) + tv(ru.ru_stime)
    }
    print("=== shm vs P8 A/B (each module \(Int(secs))s, app-side CPU + fps) ===")
    print(String(format: "%-16@  %-22@  %-22@", "module" as NSString,
                 "SHM (frames cpu fps)" as NSString, "P8 (frames cpu fps)" as NSString))
    for id in mods {
        guard let m = AD_MODULES.first(where: { $0.id == id }) else { print("\(id): NOT IN CATALOG"); continue }
        let c0 = cpuSeconds()
        let shm = EmulatedHost.smokeShmTest(module: m, seconds: secs, shm: true)
        let cShm = cpuSeconds() - c0
        let c1 = cpuSeconds()
        let p8 = EmulatedHost.smokeShmTest(module: m, seconds: secs, shm: false)
        let cP8 = cpuSeconds() - c1
        print(String(format: "%-16@  %5d %5.2fs %5.1f (act=%@)   %5d %5.2fs %5.1f",
                     id as NSString, shm.consumed, cShm, Double(shm.consumed)/secs,
                     (shm.shmActive ? "shm" : "FALLBACK") as NSString,
                     p8.consumed, cP8, Double(p8.consumed)/secs))
    }
    exit(0)
}

// --verify: headless functional check of the settings wiring (no Metal needed).
// Builds Flying Toasters scenes under different control values and reports the
// resulting scene-graph state.
if moduleId == "--verify" {
    // Catalog summary.
    let all = AD_MODULES
    let ppc = all.filter { $0.family == .ppc }
    let k68 = all.filter { $0.family == .k68 }
    print("=== catalog ===")
    print("total=\(all.count)  PPC=\(ppc.count) (expect 18)  68K=\(k68.count) (expect 62)")
    let withAbout = all.filter { ($0.about?.isEmpty == false) }.count
    let totalControls = all.reduce(0) { $0 + $1.controls.count }
    print("controls parsed total=\(totalControls)  modules with About=\(withAbout)/\(all.count)")
    let noControls = all.filter { $0.controls.isEmpty }
    print("modules with 0 controls=\(noControls.count): \(noControls.map { $0.name }.joined(separator: ", "))")
    // recipe sanity: every module resolves a host + module file.
    var badRecipe: [String] = []
    for m in all {
        guard let r = m.recipe else { badRecipe.append("\(m.name)(no recipe)"); continue }
        if !FileManager.default.fileExists(atPath: r.host) { badRecipe.append("\(m.name)(no host)") }
        let modFile = r.dataForkPath ?? r.rsrcPath
        if !FileManager.default.fileExists(atPath: modFile) { badRecipe.append("\(m.name)(no file)") }
    }
    print("recipe issues=\(badRecipe.count)\(badRecipe.isEmpty ? "" : ": " + badRecipe.joined(separator: ", "))")
    exit(0)
}

// Every module runs the real emulated code; there is no scene to render offscreen.
// The real modes are above (--smoke, --verify, --verify-emulation, ...).
FileHandle.standardError.write("adrender: unknown mode. Use --smoke <module> <secs>, --verify, or --verify-emulation.\n".data(using: .utf8)!)
exit(2)
