import Foundation
import Darwin   // getpwuid/getuid for the real-home sidecar fallback

// Minimal module model for the .saver. This is a deliberately SLIM port of the
// app's AfterDarkKit model (Modules.swift / Controls.swift / ADCatalog.swift):
// those files `import SpriteKit`/`Combine` and reference native SpriteKit scenes +
// SwiftUI stores that a screen saver has no use for. EmulatedHost.swift and
// ADPaths.swift — the load-bearing spawn/decode/path logic — are reused VERBATIM
// (compiled straight from the app source tree by build_saver.sh) and depend only
// on the tiny shapes below, which mirror the app's field-for-field so the exact
// same emulation contract runs.

public enum ADFamily: String, Hashable {
    case ppc = "PPC"        // After Dark 4.0 (PowerPC)
    case k68 = "68K"        // After Dark Deluxe (68K)
}

// How to launch a module under emulation (built from the catalog). Field-identical
// to the app's ADRecipe so EmulatedHost binds against it unchanged.
public struct ADRecipe: Hashable {
    public let host: String
    public let workingDir: String
    public let env: [String: String]
    public let argsTemplate: [String]
    public let rsrcPath: String
    public let dataForkPath: String?
    public let lane: String?
    public let controlEnvVar: String
}

// A single module control. Only the fields EmulatedHost reads are kept.
public struct ADControl: Identifiable, Hashable {
    public enum Kind: Hashable {
        case slider(min: Int, max: Int)
        case toggle
        case popup(options: [String])
        case button
    }
    public let id: String
    public let label: String
    public let kind: Kind
    public let defaultValue: Int
    public let resourceId: Int
    public let valueNames: [String]?
}

// A screensaver module the saver can display.
public struct ADModule: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let available: Bool
    public let family: ADFamily
    public let recipe: ADRecipe?
    public let controls: [ADControl]
    public let about: String?
    public let credits: String?
    public static func == (l: ADModule, r: ADModule) -> Bool { l.id == r.id }
    public func hash(into h: inout Hasher) { h.combine(id) }
}

// Loads catalog.json (bundled inside AfterDark.saver/Contents/Resources) into the
// module roster, resolving relative recipe paths through ADPaths — the SAME
// resolution the app uses, so the emulation contract is byte-identical. The paths
// ADPaths reads (assetsRoot / sharedLibsRoot / hostsDir) come from the app-group
// handoff installed by ADSaverBootstrap BEFORE this loads.
public enum ADSaverCatalog {
    private struct JControl: Decodable {
        let id: String; let resourceId: Int?; let type: String; let label: String
        let defaultValue: Int; let min: Int?; let max: Int?
        let options: [String]?; let valueNames: [String]?
    }
    private struct JModule: Decodable {
        let id: String; let displayName: String; let family: String
        let native: Bool?; let host: String?; let workingDir: String?
        let env: [String: String]?; let argsTemplate: [String]?
        let rsrcPath: String?; let dataForkPath: String?
        let lane: String?; let controlEnvVar: String?
        let controls: [JControl]; let about: String?; let credits: String?
    }
    private struct JCatalog: Decodable { let modules: [JModule] }

    public static func load(from bundle: Bundle) -> [ADModule] {
        guard let url = bundle.url(forResource: "catalog", withExtension: "json")
                ?? bundle.url(forResource: "catalog", withExtension: "json", subdirectory: "Resources"),
              let data = try? Data(contentsOf: url),
              let cat = try? JSONDecoder().decode(JCatalog.self, from: data)
        else { return [] }
        return cat.modules.map { jm in
            let fam: ADFamily = (jm.family == "68K") ? .k68 : .ppc
            // Guarantee a unique control id (two controls can share a resource ID
            // across resource types — Swirling Magic's mVal/xVal 1002); mirrors the
            // app's collision-suffixing so per-control identity/persistence works.
            var seen: [String: Int] = [:]
            let controls: [ADControl] = jm.controls.map { c in
                let kind: ADControl.Kind
                switch c.type {
                case "toggle": kind = .toggle
                case "popup":  kind = .popup(options: c.options ?? [])
                case "button": kind = .button
                default:       kind = .slider(min: c.min ?? 0, max: c.max ?? 100)
                }
                let n = (seen[c.id] ?? 0) + 1
                seen[c.id] = n
                let uid = n == 1 ? c.id : "\(c.id)#\(n)"
                return ADControl(id: uid, label: c.label, kind: kind,
                                 defaultValue: c.defaultValue,
                                 resourceId: c.resourceId ?? 0,
                                 valueNames: c.valueNames)
            }
            var recipe: ADRecipe? = nil
            if let host = jm.host, jm.workingDir != nil,
               let rsrc = jm.rsrcPath, let args = jm.argsTemplate {
                recipe = ADRecipe(host: ADPaths.resolveHost(host),
                                  workingDir: ADPaths.sharedLibsRoot,
                                  env: jm.env ?? [:], argsTemplate: args,
                                  rsrcPath: ADPaths.resolveAsset(rsrc),
                                  dataForkPath: jm.dataForkPath.map(ADPaths.resolveAsset),
                                  lane: jm.lane,
                                  controlEnvVar: jm.controlEnvVar ?? "ADCVSET")
            }
            return ADModule(id: jm.id, name: jm.displayName, available: true,
                            family: fam, recipe: recipe,
                            controls: controls, about: jm.about, credits: jm.credits)
        }
    }
}

// Bridges the app-group asset handoff into ADPaths' environment overrides. MUST run
// before anything touches ADPaths / ADSaverCatalog (ADPaths' roots are `static let`,
// evaluated once on first access). Returns whether a usable handoff was found so the
// view can show the "open the app first" frame when it wasn't.
public enum ADSaverBootstrap {
    // Same group as the app's ADGroupHandoff.suiteName.
    public static let suiteName = "group.com.afterdark.modern"

    // durationSeconds is the app's global Duration preference,
    // carried on the same two channels as the paths (see below). nil = the app has
    // never published one, so the saver uses ADDuration.defaultSeconds (60 s = the
    // authentic sVal 503 factory default) and nothing changes.
    public struct Result {
        public let haveAssets: Bool
        public let assetsRoot: String?
        public let sharedLibsRoot: String?
        public let durationSeconds: Int?
    }

    // hostsDir: the saver bundles its own signed adhost/adhost68k in Contents/Resources
    // (self-contained install), so AD_HOST_DIR always points there — independent of
    // whatever hostsDir the app happened to publish.
    @discardableResult
    public static func installEnv(bundledHostsDir: String) -> Result {
        setenv("AD_HOST_DIR", bundledHostsDir, 1)

        let fm = FileManager.default

        // Channel 1: the app-group suite. Works when the reader shares the app's home
        // (unsandboxed dev / harness) or genuinely carries the group entitlement.
        let d = UserDefaults(suiteName: suiteName)
        var assets = d?.string(forKey: "assetsRoot")
        var shared = d?.string(forKey: "sharedLibsRoot")
        // 0 (the `object(forKey:)`-less default) is not a stop, so a
        // suite that has never carried a duration reads as nil and falls through.
        var duration = d?.object(forKey: "durationSeconds") as? Int

        // Channel 2 (robust in the real appex): a JSON sidecar the app writes at a
        // FIXED absolute path under the real user's app-support dir. Inside the
        // screensaver appex our $HOME is redirected AND an MH_BUNDLE .saver cannot
        // carry the app-group entitlement, so channel 1 misses; getpwuid() still
        // returns the REAL home, and the appex's whole-filesystem READ exception
        // lets us open the sidecar by absolute path.
        // `|| duration == nil` makes the sidecar also get consulted for the Duration
        // preference even on a machine where channel 1 happened to serve the paths.
        // Both fields come from the same publish(), so a sidecar read can only ever
        // confirm the paths, never contradict them.
        if assets == nil || assets?.isEmpty == true || !fm.fileExists(atPath: assets ?? "/nonexistent")
            || duration == nil {
            if let realHome = Self.realHomeDir() {
                let sidecar = realHome + "/Library/Application Support/AfterDarkModern/saver-handoff.json"
                if let data = fm.contents(atPath: sidecar),
                   let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: String] {
                    if let a = obj["assetsRoot"], !a.isEmpty { assets = a }
                    if let s = obj["sharedLibsRoot"], !s.isEmpty { shared = s }
                    if let v = obj["durationSeconds"], let n = Int(v) { duration = n }
                }
            }
        }

        if let assets, !assets.isEmpty { setenv("AD_ASSETS_DIR", assets, 1) }
        if let shared, !shared.isEmpty { setenv("AD_SHARED_DIR", shared, 1) }

        let have = (assets?.isEmpty == false)
                && fm.fileExists(atPath: assets ?? "/nonexistent")
        return Result(haveAssets: have, assetsRoot: assets, sharedLibsRoot: shared,
                      durationSeconds: duration)
    }

    // The REAL user home, unaffected by the appex's $HOME/NSHomeDirectory redirection.
    private static func realHomeDir() -> String? {
        if let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir {
            return String(cString: dir)
        }
        return nil
    }
}

// The saver's working copy of the shared settings document (ADSharedSettings —
// compiled into this bundle from the app source tree). Loaded once per process,
// mutated by the configure sheet, saved mirror-first (the appex container is always
// writable; the primary write is best-effort and succeeds wherever the sandbox
// allows it — the app absorbs the mirror either way).
public final class ADSaverSettings {
    public private(set) var doc: ADSharedSettings

    public init() {
        doc = ADSharedSettings.loadForSaver()
    }

    // Effective duration: shared document, else the app's sidecar publish, else the
    // authentic factory default.
    public func durationSeconds(sidecarFallback: Int?) -> Int {
        ADDuration.sanitize(doc.durationSeconds ?? sidecarFallback)
    }

    // Effective control values for a module, keyed by control id (what EmulatedHost
    // expects). Unset controls fall back to their factory defaults.
    public func snapshot(for module: ADModule) -> [String: Int] {
        var out: [String: Int] = [:]
        for c in module.controls {
            out[c.id] = doc.value(module: module.id, control: c.id) ?? c.defaultValue
        }
        return out
    }

    public func value(for module: ADModule, control: ADControl) -> Int {
        doc.value(module: module.id, control: control.id) ?? control.defaultValue
    }

    public func set(_ v: Int, for module: ADModule, control: ADControl) {
        doc.set(v, module: module.id, control: control.id)
    }

    public func resetModule(_ module: ADModule) {
        doc.clearModule(module.id)
    }

    public func setDuration(_ seconds: Int) {
        doc.setDuration(seconds)
    }

    public func setSelection(_ moduleId: String) {
        doc.setSelection(moduleId)
    }

    public var selection: String? { doc.selectedModule }

    public var randomizerSet: [String]? { doc.randomizerSet }

    public func setRandomizerSet(_ ids: [String]?) {
        doc.setRandomizerSet(ids)
    }

    public func save() {
        ADSharedSettings.saveFromSaver(doc)
    }

    public func reload() {
        doc = ADSharedSettings.loadForSaver()
    }
}
