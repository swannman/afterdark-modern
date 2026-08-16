import Foundation
import Darwin   // getpwuid/getuid — the REAL home, unaffected by appex $HOME redirection

// The shared settings document: ONE store both the app and AfterDark.saver read and
// write, so a value changed in either place governs both. It carries the global
// Duration preference and every per-module control value (flat "moduleId.controlId"
// keys, the same shape ADSettingsStore always persisted).
//
// Cross-process mechanics (the same trick as the asset-handoff sidecar):
//   * primary file — <real home>/Library/Application Support/AfterDarkModern/settings.json.
//     The app reads/writes it freely. The saver (inside legacyScreenSaver.appex, whose
//     $HOME is redirected) reads it by absolute path through the appex's filesystem
//     read exception, and ATTEMPTS to write it — the appex may or may not allow that.
//   * mirror file — the same document under NSHomeDirectory(), which inside the appex
//     is the appex's own container (always writable). Every saver-side save lands here
//     unconditionally, so no edit is ever lost even when the primary write is denied.
//   * merge — per-module timestamps + a duration timestamp; newer wins. The app
//     absorbs the appex-container mirror on launch/activation and folds it back into
//     the primary, which closes the saver -> app loop.
public struct ADSharedSettings: Codable {
    public var durationSeconds: Int?
    public var durationStamp: Double
    // The saver's module selection: a module id, "__cycle_all__", or nil (never
    // chosen -> cycle-all). Lives here — NOT in ScreenSaverDefaults — because the
    // sheet host and the running saver can be different processes with different
    // preference sandboxes; this document is the one store every process shares.
    public var selectedModule: String?
    public var selectionStamp: Double?
    // Flat control values, "moduleId.controlId" -> value (ADSettingsStore's shape).
    public var values: [String: Int]
    // moduleId -> last edit time for that module's values.
    public var moduleStamps: [String: Double]

    public init(durationSeconds: Int? = nil, durationStamp: Double = 0,
                values: [String: Int] = [:], moduleStamps: [String: Double] = [:]) {
        self.durationSeconds = durationSeconds
        self.durationStamp = durationStamp
        self.values = values
        self.moduleStamps = moduleStamps
    }

    // MARK: - Locations

    public static func realHome() -> String? {
        if let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir { return String(cString: dir) }
        return nil
    }

    private static let relPath = "/Library/Application Support/AfterDarkModern/settings.json"
    private static let mirrorRelPath = "/Library/Application Support/AfterDarkModern/saver-settings.json"
    private static let appexContainer = "/Library/Containers/com.apple.ScreenSaver.Engine.legacyScreenSaver/Data"

    // The primary document (real user home — same file from every process).
    public static func primaryPath() -> String? { realHome().map { $0 + relPath } }

    // The saver-side mirror. Inside the appex NSHomeDirectory() is the container, so
    // this is always writable there; in an unsandboxed process it lands in the real
    // Application Support dir under its distinct name.
    public static func mirrorPath() -> String { NSHomeDirectory() + mirrorRelPath }

    // Where the app finds the appex-container mirror to absorb saver-side edits.
    public static func appexMirrorPaths() -> [String] {
        guard let home = realHome() else { return [] }
        return [home + appexContainer + mirrorRelPath,   // written inside the real appex
                home + mirrorRelPath]                    // written by dev/harness runs
    }

    // MARK: - IO

    public static func load(_ path: String) -> ADSharedSettings? {
        guard let data = FileManager.default.contents(atPath: path) else { return nil }
        return try? JSONDecoder().decode(ADSharedSettings.self, from: data)
    }

    @discardableResult
    public static func write(_ doc: ADSharedSettings, to path: String) -> Bool {
        guard let data = try? JSONEncoder().encode(doc) else { return false }
        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        do { try data.write(to: URL(fileURLWithPath: path), options: .atomic); return true }
        catch { return false }
    }

    // MARK: - Merge (newer wins, per module + duration)

    public static func merged(_ docs: [ADSharedSettings?]) -> ADSharedSettings {
        var out = ADSharedSettings()
        for maybe in docs {
            guard let d = maybe else { continue }
            if d.durationSeconds != nil, d.durationStamp >= out.durationStamp {
                out.durationSeconds = d.durationSeconds
                out.durationStamp = d.durationStamp
            }
            if d.selectedModule != nil, (d.selectionStamp ?? 0) >= (out.selectionStamp ?? -1) {
                out.selectedModule = d.selectedModule
                out.selectionStamp = d.selectionStamp ?? 0
            }
            for (mod, stamp) in d.moduleStamps where stamp >= (out.moduleStamps[mod] ?? -1) {
                out.moduleStamps[mod] = stamp
                let prefix = mod + "."
                // Replace the module's values wholesale with the newer document's.
                out.values = out.values.filter { !$0.key.hasPrefix(prefix) }
                for (k, v) in d.values where k.hasPrefix(prefix) { out.values[k] = v }
            }
        }
        return out
    }

    // MARK: - Accessors

    public func value(module: String, control: String) -> Int? { values["\(module).\(control)"] }

    public mutating func set(_ v: Int, module: String, control: String, at time: Double = Date().timeIntervalSince1970) {
        values["\(module).\(control)"] = v
        moduleStamps[module] = time
    }

    public mutating func clearModule(_ module: String, at time: Double = Date().timeIntervalSince1970) {
        let prefix = module + "."
        values = values.filter { !$0.key.hasPrefix(prefix) }
        moduleStamps[module] = time
    }

    public mutating func setDuration(_ seconds: Int, at time: Double = Date().timeIntervalSince1970) {
        durationSeconds = seconds
        durationStamp = time
    }

    public mutating func setSelection(_ moduleId: String, at time: Double = Date().timeIntervalSince1970) {
        selectedModule = moduleId
        selectionStamp = time
    }

    // MARK: - Process-level load/save

    // App side: primary + appex mirrors, newest wins; fold the result back into the
    // primary so the saver's next read sees everything in one place.
    public static func loadForApp() -> ADSharedSettings {
        var docs: [ADSharedSettings?] = [primaryPath().flatMap(load)]
        docs += appexMirrorPaths().map(load)
        let doc = merged(docs)
        if let p = primaryPath() { write(doc, to: p) }
        return doc
    }

    // Saver side: primary + own mirror, newest wins.
    public static func loadForSaver() -> ADSharedSettings {
        merged([primaryPath().flatMap(load), load(mirrorPath())])
    }

    // Saver side: mirror always (container — always writable), primary best-effort.
    public static func saveFromSaver(_ doc: ADSharedSettings) {
        write(doc, to: mirrorPath())
        if let p = primaryPath() { write(doc, to: p) }
    }

    // App side.
    public static func saveFromApp(_ doc: ADSharedSettings) {
        if let p = primaryPath() { write(doc, to: p) }
    }
}
