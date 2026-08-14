import Foundation
import Combine

// Scenes that can consume module control values live (without a rebuild).

// Module settings model. Mirrors the control typing of real After Dark modules
// (see tools/adhost/adhost68k.cc ~5752: 'sVal' slider / 'xVal' checkbox /
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

// Persisted per-module control values ("<moduleId>.<controlId>" -> Int).
public final class ADSettingsStore: ObservableObject {
    @Published public private(set) var values: [String: Int]
    private let defaultsKey = "ADModuleSettings"

    public init() {
        values = (UserDefaults.standard.dictionary(forKey: defaultsKey) as? [String: Int]) ?? [:]
    }

    private func key(_ module: ADModule, _ control: ADControl) -> String {
        "\(module.id).\(control.id)"
    }

    public func value(for module: ADModule, control: ADControl) -> Int {
        values[key(module, control)] ?? control.defaultValue
    }

    public func set(_ v: Int, for module: ADModule, control: ADControl) {
        values[key(module, control)] = v
        UserDefaults.standard.set(values, forKey: defaultsKey)
    }

    // Snapshot of a module's effective control values keyed by control id.
    public func snapshot(for module: ADModule) -> [String: Int] {
        var out: [String: Int] = [:]
        for c in module.controls { out[c.id] = value(for: module, control: c) }
        return out
    }
}

// addm 796: the GLOBAL Duration preference (the control panel's own slider,
// see ADDuration for the resource provenance). Unlike ADSettingsStore this is not
// per-module — Duration belonged to the control panel, not to any module.
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
            EmulatedHost.durationSeconds = seconds
            ADGroupHandoff.publish()
        }
    }

    public var stop: ADDuration.Stop { ADDuration.stop(forSeconds: seconds) }

    public init() {
        seconds = ADDuration.sanitize(UserDefaults.standard.object(forKey: ADDuration.defaultsKey) as? Int)
        // didSet does not run for the initial assignment, so arm the host explicitly.
        EmulatedHost.durationSeconds = seconds
    }
}
