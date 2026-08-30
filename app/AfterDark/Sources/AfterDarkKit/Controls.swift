import Foundation
import Combine

// Scenes that can consume module control values live (without a rebuild).

// Module settings model. Mirrors the control typing of real After Dark modules
// (see engine/adhost68k.cc ~5752: 'sVal' slider / 'xVal' checkbox /
// 'mVal' popup menu), so ported modules can expose the same controls the
// originals defined.
public struct ADControl: Identifiable, Hashable {
    public enum Kind: Hashable {
        case slider(min: Int, max: Int)      // 'sVal'
        case toggle                          // 'xVal'
        case popup(options: [String])        // 'mVal'
        case button                          // 'bVal' (opens a dialog in the original — inert here)
    }
    public let id: String
    public let label: String
    public let kind: Kind
    public let defaultValue: Int
    // The original resource ID (1000-based). The emulation host addresses a
    // module's control values by index = resourceId - 1000.
    public let resourceId: Int
    // Optional display names for slider positions (e.g. Light...Dark); when set,
    // the value readout shows the name instead of the number.
    public let valueNames: [String]?

    public init(id: String, label: String, kind: Kind, defaultValue: Int,
                resourceId: Int = 0, valueNames: [String]? = nil) {
        self.id = id; self.label = label; self.kind = kind
        self.defaultValue = defaultValue; self.resourceId = resourceId
        self.valueNames = valueNames
    }

    public func displayValue(_ v: Int) -> String {
        if let names = valueNames, v >= 0, v < names.count { return names[v] }
        if case .popup(let options) = kind, v >= 0, v < options.count { return options[v] }
        if case .toggle = kind { return v != 0 ? "On" : "Off" }
        return String(v)
    }
}

// The app's single in-memory copy of the shared settings document (see
// ADSharedSettings). Both stores below mutate THIS copy under one lock, so a
// duration write can never clobber a control write with a stale document.
enum ADSharedDocBox {
    private static let lock = NSLock()
    private static var doc = ADSharedSettings.loadForApp()
    static func with<T>(_ f: (inout ADSharedSettings) -> T) -> T {
        lock.lock(); defer { lock.unlock() }
        let r = f(&doc)
        ADSharedSettings.saveFromApp(doc)
        return r
    }
    // Re-read from disk (absorbing saver-side edits); returns the merged document.
    static func reload() -> ADSharedSettings {
        lock.lock(); defer { lock.unlock() }
        doc = ADSharedSettings.loadForApp()
        return doc
    }
    static func snapshot() -> ADSharedSettings {
        lock.lock(); defer { lock.unlock() }
        return doc
    }
}

// Persisted per-module control values ("<moduleId>.<controlId>" -> Int), backed by
// the SHARED settings document so the app and AfterDark.saver see each other's
// edits (newer edit wins, per module).
public final class ADSettingsStore: ObservableObject {
    @Published public private(set) var values: [String: Int]
    private let defaultsKey = "ADModuleSettings"

    public init() {
        // One-time migration: fold any pre-shared-document defaults into the
        // document, then serve from the document from here on.
        let legacy = (UserDefaults.standard.dictionary(forKey: defaultsKey) as? [String: Int]) ?? [:]
        let doc = ADSharedDocBox.with { doc in
            if doc.values.isEmpty, !legacy.isEmpty {
                for (k, v) in legacy {
                    if let dot = k.firstIndex(of: ".") {
                        doc.set(v, module: String(k[..<dot]), control: String(k[k.index(after: dot)...]))
                    }
                }
            }
            return doc
        }
        values = doc.values
    }

    private func key(_ module: ADModule, _ control: ADControl) -> String {
        "\(module.id).\(control.id)"
    }

    public func value(for module: ADModule, control: ADControl) -> Int {
        values[key(module, control)] ?? control.defaultValue
    }

    public func set(_ v: Int, for module: ADModule, control: ADControl) {
        let doc = ADSharedDocBox.with { $0.set(v, module: module.id, control: control.id); return $0 }
        values = doc.values
        UserDefaults.standard.set(values, forKey: defaultsKey)
    }

    // Absorb edits made in the screen saver's configure sheet (called on app
    // activation; no-op when nothing changed).
    public func reloadFromDisk() {
        let doc = ADSharedDocBox.reload()
        if doc.values != values { values = doc.values }
    }

    // Snapshot of a module's effective control values keyed by control id.
    public func snapshot(for module: ADModule) -> [String: Int] {
        var out: [String: Int] = [:]
        for c in module.controls { out[c.id] = value(for: module, control: c) }
        return out
    }
}

// The GLOBAL Duration preference (the control panel's own slider, see ADDuration
// for the resource provenance). Unlike ADSettingsStore this is not per-module —
// Duration belongs to the control panel, not to any module.
//
// Owns the three places the value has to land: the app's defaults domain (persistence),
// EmulatedHost.durationSeconds (every host this process spawns from now on), and the
// asset-handoff sidecar (how the .saver, in another process with another defaults
// domain, learns the same number).
public final class ADDurationStore: ObservableObject {
    @Published public var seconds: Int {
        didSet {
            guard seconds != oldValue else { return }
            UserDefaults.standard.set(seconds, forKey: ADDuration.defaultsKey)
            ADGroupHandoff.publish()
            // Don't re-stamp the document when the new value CAME from it.
            if !reloading {
                ADSharedDocBox.with { $0.setDuration(seconds) }
            }
        }
    }
    private var reloading = false

    public var stop: ADDuration.Stop { ADDuration.stop(forSeconds: seconds) }

    public init() {
        // The shared document wins over the app-local default (it carries saver-side
        // edits); fall back to the legacy defaults key, then the factory default.
        let doc = ADSharedDocBox.snapshot()
        seconds = ADDuration.sanitize(doc.durationSeconds
            ?? UserDefaults.standard.object(forKey: ADDuration.defaultsKey) as? Int)
        // Duration is the Randomizer's switching interval (the saver's Randomize
        // rotation reads it from the shared document). A single running module —
        // including every app preview — is never restarted, as in the original,
        // so the host cycle stays permanently disarmed here.
        EmulatedHost.durationSeconds = ADDuration.forever
    }

    // Absorb a Duration change made in the saver's configure sheet.
    public func reloadFromDisk() {
        let doc = ADSharedDocBox.reload()
        if let d = doc.durationSeconds, ADDuration.sanitize(d) != seconds {
            reloading = true
            seconds = ADDuration.sanitize(d)
            reloading = false
        }
    }
}
