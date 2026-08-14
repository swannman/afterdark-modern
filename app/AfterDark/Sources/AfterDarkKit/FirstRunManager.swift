import Foundation
import SwiftUI

// Reference-type line accumulator for a pipe's readabilityHandler: append raw
// bytes, return the complete newline-terminated lines drained so far.
final class LineBuffer: @unchecked Sendable {
    private var buf = Data()
    private let lock = NSLock()
    func appendAndDrainLines(_ d: Data) -> [String] {
        lock.lock(); defer { lock.unlock() }
        buf.append(d)
        var lines: [String] = []
        while let nl = buf.firstIndex(of: 0x0A) {
            let line = buf.subdata(in: buf.startIndex..<nl)
            buf.removeSubrange(buf.startIndex...nl)
            if let s = String(data: line, encoding: .utf8) { lines.append(s) }
        }
        return lines
    }
}

// Drives the first-run acquisition of the original After Dark asset tree.
//
// The app ships NONE of the copyrighted bits. On first launch, if the asset tree
// the emulation hosts read (ADPaths.assetsRoot) is missing, this manager gets the
// user's consent and then drives the proven tools/adfetch/adfetch.sh to download
// the sources from the Internet Archive and lay the tree out under Application
// Support — exactly where ADPaths.assetsRoot already points.
//
// Idiomatic macOS locations (hard constraint — the .app may run read-only /
// translocated from /Applications, so nothing is ever written near the bundle):
//   * extracted tree (kept):        ~/Library/Application Support/AfterDarkModern/assets
//   * raw downloads (re-fetchable):  ~/Library/Caches/AfterDarkModern
//                                    deleted after a successful verify.
//   * bundled tools (adfetch +
//     unar/hfsutils):                inside the .app (Contents/Helpers), read-only.
@MainActor
public final class FirstRunManager: ObservableObject {
    public enum Stage: Equatable {
        case checking                 // deciding whether the tree is already present
        case needsConsent             // tree missing, extract tools present — ask
        case needsTools(String)       // extract tools missing — show install guidance
        case downloading              // adfetch is running
        case ready                    // tree present/valid — show the app
        case failed(String)           // adfetch failed — message + retry
    }

    @Published public private(set) var stage: Stage = .checking
    @Published public private(set) var phaseLabel: String = ""   // human phase name
    @Published public private(set) var pct: Double = 0           // 0…1 overall
    @Published public private(set) var byteLabel: String = ""    // "123.4 MB" during a download
    @Published public private(set) var lastLog: String = ""      // last human [adfetch] line

    // Space we require free before starting: ~450 MB of raw downloads plus the
    // extracted tree and peak intermediate staging. 2 GB is a safe floor.
    private static let requiredFreeBytes: Int64 = 2 * 1024 * 1024 * 1024

    private let assetsRoot: String
    private var cacheDir: String {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
        return (base?.appendingPathComponent("AfterDarkModern").path)
            ?? NSTemporaryDirectory() + "AfterDarkModern-cache"
    }
    private var sentinelURL: URL { URL(fileURLWithPath: assetsRoot).appendingPathComponent(".adfetch-complete") }

    private var process: Process?
    private var stderrTail = Data()

    public init() { self.assetsRoot = ADPaths.assetsRoot }

    // Test seam: pin the assets root to a temp dir so presence/structure detection
    // can be exercised without touching the real Application Support tree.
    init(assetsRootOverride: String) { self.assetsRoot = assetsRootOverride }

    // MARK: - Presence detection

    // Recheck whether the tree is present; drive the stage machine. Called at
    // launch and after a "Cancel/Quit was declined then retried" style re-entry.
    public func refresh() {
        if ProcessInfo.processInfo.environment["AD_SKIP_FIRSTRUN"] != nil { stage = .ready; return }
        if assetsPresent() { stage = .ready; return }
        if let missing = missingTools() { stage = .needsTools(missing); return }
        stage = .needsConsent
    }

    // Fast-path presence check (no hashing): our own successful runs drop a
    // sentinel; otherwise accept a structurally complete tree so an existing
    // dev tree / symlink (ADPaths.assetsRoot -> a populated checkout) coexists
    // and is never re-downloaded. The full byte-level check is adfetch's own
    // verify_manifest, which runs at the END of a download and gates the sentinel.
    public func assetsPresent() -> Bool {
        let fm = FileManager.default
        if fm.fileExists(atPath: sentinelURL.path) { return true }
        // Structural check: both module trees exist and are non-empty.
        let ppcTree = "\(assetsRoot)/extracted/After Dark 9 v1.0 (9:9:03)/After Dark Files/After Dark 4.0"
        let k68Tree = "\(assetsRoot)/deluxe/extracted/dlx/After Dark Deluxe (4"
        return dirHasEntries(ppcTree, fm) && dirHasEntries(k68Tree, fm)
    }

    private func dirHasEntries(_ path: String, _ fm: FileManager) -> Bool {
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else { return false }
        let n = (try? fm.contentsOfDirectory(atPath: path).count) ?? 0
        return n > 0
    }

    // MARK: - Tool detection
    // A GUI app launched from /Applications inherits a minimal PATH (no Homebrew),
    // so we search known locations directly rather than trusting $PATH. Order:
    // the tools we bundle and sign ourselves first, then Homebrew/system bins for
    // a dev checkout run outside a built .app.
    nonisolated private static let toolSearchDirs: [String] = {
        var dirs: [String] = []
        if let bundled = bundledToolsDir { dirs.append(bundled) }
        dirs += ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/sbin"]
        return dirs
    }()

    // make_app.sh puts unar + hfsutils here; nil when running from a dev build
    // that isn't a .app bundle.
    nonisolated static let bundledToolsDir: String? = toolsDir(inBundleAt: Bundle.main.bundlePath)

    // unar is the marker: if it isn't there and runnable, this isn't a bundle
    // that carries its own extraction tools.
    nonisolated static func toolsDir(inBundleAt bundlePath: String) -> String? {
        let dir = "\(bundlePath)/Contents/Helpers/bin"
        return FileManager.default.isExecutableFile(atPath: "\(dir)/unar") ? dir : nil
    }

    nonisolated static func locate(_ tool: String) -> String? {
        let fm = FileManager.default
        for d in toolSearchDirs {
            let p = "\(d)/\(tool)"
            if fm.isExecutableFile(atPath: p) { return p }
        }
        return nil
    }

    // The IA fetch path needs unar (unpack) + hfsutils (read the HFS/ISO images);
    // curl comes with macOS. A built .app carries all of them in
    // Contents/Helpers/bin, so this only ever fires for a dev run outside the
    // bundle. Returns a user-facing guidance string if any are missing, else nil.
    private func missingTools() -> String? {
        let need = ["unar", "hmount", "hcd", "hls", "hcopy", "humount", "curl"]
        let absent = need.filter { Self.locate($0) == nil }
        guard !absent.isEmpty else { return nil }
        // Map the missing binaries back to their Homebrew formulae.
        var formulae: Set<String> = []
        for t in absent {
            switch t {
            case "unar": formulae.insert("unar")
            case "hmount", "hcd", "hls", "hcopy", "humount": formulae.insert("hfsutils")
            default: break   // curl ships with macOS; can't brew-install cleanly
            }
        }
        let missingList = absent.joined(separator: ", ")
        if formulae.isEmpty {
            return "Missing required tools: \(missingList). These normally ship with macOS "
                 + "(install the Xcode Command Line Tools with: xcode-select --install)."
        }
        return "The downloader needs these tools to unpack the archives, and they were "
             + "not found: \(missingList).\n\nInstall them with Homebrew:\n"
             + "    brew install \(formulae.sorted().joined(separator: " "))\n\n"
             + "Then quit and reopen After Dark."
    }

    // MARK: - adfetch location
    private func adfetchScript() -> String? {
        let fm = FileManager.default
        // 1. Bundled inside the .app (Contents/Resources/adfetch/adfetch.sh).
        let bundled = "\(Bundle.main.bundlePath)/Contents/Resources/adfetch/adfetch.sh"
        if fm.fileExists(atPath: bundled) { return bundled }
        // 2. Dev checkout: repoRoot/tools/adfetch/adfetch.sh.
        let dev = "\(ADPaths.repoRoot)/tools/adfetch/adfetch.sh"
        if fm.fileExists(atPath: dev) { return dev }
        return nil
    }

    // MARK: - Download drive

    public func startDownload() {
        guard case .downloading = stage else {
            _startDownload()
            return
        }
    }

    private func _startDownload() {
        // Disk-space preflight (check the volume backing Application Support).
        if let free = volumeFreeBytes(forPath: (assetsRoot as NSString).deletingLastPathComponent),
           free < Self.requiredFreeBytes {
            let needGB = Double(Self.requiredFreeBytes) / 1_073_741_824
            let haveGB = Double(free) / 1_073_741_824
            stage = .failed(String(format: "Not enough free disk space. Need about %.1f GB free, "
                                          + "but only %.1f GB is available.", needGB, haveGB))
            return
        }
        guard let script = adfetchScript() else {
            stage = .failed("Could not locate the downloader (adfetch.sh). The app may be "
                          + "incomplete — try reinstalling.")
            return
        }
        try? FileManager.default.createDirectory(atPath: cacheDir, withIntermediateDirectories: true)

        stage = .downloading
        phaseLabel = "Starting…"; pct = 0; byteLabel = ""; lastLog = ""; stderrTail = Data()

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [script,
                       "--assets", assetsRoot,
                       "--cache", cacheDir,
                       "--progress",
                       "--cleanup-downloads"]
        var env = ProcessInfo.processInfo.environment
        env["AD_ASSETS_DIR"] = assetsRoot
        env["AD_CACHE_DIR"] = cacheDir
        env["ADFETCH_USE_IA"] = "1"   // Internet Archive sources (content-verified)
        // Point adfetch at the tools we bundle and sign, which it puts ahead of
        // everything on PATH — the app's extraction must not depend on what the
        // user happens to have installed.
        if let bundled = Self.bundledToolsDir { env["AD_TOOLS_DIR"] = bundled }
        // Give the child a PATH that actually contains Homebrew + the tools we
        // located, since a GUI-launched app's PATH is minimal.
        var pathDirs = Self.toolSearchDirs
        pathDirs += (env["PATH"] ?? "").split(separator: ":").map(String.init)
        env["PATH"] = pathDirs.joined(separator: ":")
        p.environment = env

        let outPipe = Pipe(); let errPipe = Pipe()
        p.standardOutput = outPipe
        p.standardError = errPipe

        // A reference-type accumulator so the readabilityHandler (an @Sendable
        // closure run on a private queue) can buffer partial lines without
        // capturing a mutable var.
        let outBuf = LineBuffer()
        outPipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty else { return }
            for text in outBuf.appendAndDrainLines(d) {
                Task { @MainActor in self?.handleStdout(text) }
            }
        }
        errPipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty else { return }
            Task { @MainActor in self?.handleStderr(d) }
        }

        p.terminationHandler = { [weak self] proc in
            let status = proc.terminationStatus
            Task { @MainActor in self?.handleTermination(status) }
        }

        do {
            try p.run()
            process = p
        } catch {
            stage = .failed("Could not start the downloader: \(error.localizedDescription)")
        }
    }

    public func cancel() {
        process?.terminate()
        process = nil
    }

    // MARK: - Stream handling (main actor)

    private func handleStdout(_ line: String) {
        let t = line.trimmingCharacters(in: .whitespacesAndNewlines)
        guard t.hasPrefix("PROGRESS ") else { return }
        var kv: [String: String] = [:]
        for tok in t.dropFirst("PROGRESS ".count).split(separator: " ") {
            let parts = tok.split(separator: "=", maxSplits: 1)
            if parts.count == 2 { kv[String(parts[0])] = String(parts[1]) }
        }
        if let pctStr = kv["pct"], let v = Double(pctStr) { pct = max(pct, v / 100.0) }
        if let phase = kv["phase"] { phaseLabel = Self.label(forPhase: phase) }
        if let bytesStr = kv["bytes"], let n = Int64(bytesStr) {
            byteLabel = ByteCountFormatter.string(fromByteCount: n, countStyle: .file) + " downloaded"
        } else if kv["bytes"] == nil, kv["phase"] != nil {
            byteLabel = ""   // left a download phase
        }
    }

    private func handleStderr(_ d: Data) {
        stderrTail.append(d)
        if stderrTail.count > 8192 { stderrTail.removeSubrange(0..<(stderrTail.count - 8192)) }
        if let s = String(data: d, encoding: .utf8) {
            // Surface the most recent human [adfetch] line as a status subtitle.
            for raw in s.split(separator: "\n") {
                let line = raw.trimmingCharacters(in: .whitespaces)
                if !line.isEmpty { lastLog = stripANSI(line) }
            }
        }
    }

    private func handleTermination(_ status: Int32) {
        process = nil
        if status == 0 {
            // adfetch already ran verify_manifest and exited 0 => the tree is good.
            FileManager.default.createFile(atPath: sentinelURL.path,
                                           contents: Data("ok\n".utf8))
            pct = 1.0; phaseLabel = "Done"; byteLabel = ""
            stage = .ready
        } else {
            let tail = String(data: stderrTail, encoding: .utf8).map(stripANSI) ?? ""
            let msg = tail.isEmpty
                ? "The download failed (exit code \(status))."
                : "The download failed:\n\n\(tail.suffix(600))"
            stage = .failed(msg)
        }
    }

    // MARK: - Helpers

    private func volumeFreeBytes(forPath path: String) -> Int64? {
        let url = URL(fileURLWithPath: path.isEmpty ? NSHomeDirectory() : path)
        if let vals = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let cap = vals.volumeAvailableCapacityForImportantUsage {
            return cap
        }
        // Fallback: attributesOfFileSystem on the home dir.
        if let attrs = try? FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory()),
           let free = attrs[.systemFreeSize] as? NSNumber {
            return free.int64Value
        }
        return nil
    }

    private func stripANSI(_ s: String) -> String {
        // Remove the \033[...m colour codes the [adfetch] log uses.
        guard let re = try? NSRegularExpression(pattern: "\\u{1B}\\[[0-9;]*m") else { return s }
        let r = NSRange(s.startIndex..., in: s)
        return re.stringByReplacingMatches(in: s, range: r, withTemplate: "")
    }

    nonisolated static func label(forPhase phase: String) -> String {
        switch phase {
        case "ad9-download": return "Downloading After Dark 4.0 (PowerPC)…"
        case "ad9-extract":  return "Extracting After Dark 4.0…"
        case "dlx-download": return "Downloading After Dark Deluxe…"
        case "dlx-extract":  return "Extracting After Dark Deluxe…"
        case "shared":       return "Installing shared libraries…"
        case "verify":       return "Verifying files…"
        case "done":         return "Done"
        case "error":        return "Error"
        default:             return phase
        }
    }

    // The rights/consent text, kept in sync with tools/adfetch/sources.conf.
    public nonisolated static let disclaimer = """
    After Dark and its screen saver modules are copyrighted by Berkeley Systems \
    (the After Dark brand continues via Infinisys). This app ships NONE of those \
    original files.

    If you continue, about 450 MB will be downloaded from the Internet Archive \
    (archive.org) to your Mac and stored in your Application Support folder. You \
    are responsible for your own right to download and use these files for local \
    use.
    """
}
