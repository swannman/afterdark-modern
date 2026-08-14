import Foundation

// Publishes the resolved ABSOLUTE asset/shared-lib/host paths to a shared app-group
// UserDefaults suite so the companion AfterDark.saver can find the downloaded
// modules.
//
// WHY this is needed: a macOS screen saver runs inside legacyScreenSaver.appex,
// whose $HOME is redirected to the appex's own sandbox container. So the saver
// CANNOT compute ~/Library/Application Support/AfterDarkModern/assets the way the
// app does (ADPaths) — that path resolves inside the wrong home. The whole-filesystem
// READ exception the appex carries means the saver CAN open()/read() any ABSOLUTE
// path once it knows it; the app group is how it learns the absolute path. Both
// processes derive the same suite from the same group id.
//
// The saver reads these back and feeds them to ADPaths via the AD_ASSETS_DIR /
// AD_SHARED_DIR / AD_HOST_DIR environment overrides before it builds its catalog.
public enum ADGroupHandoff {
    // App group shared between the AfterDark app and AfterDark.saver. Both are signed
    // with this group entitlement (com.apple.security.application-groups, same
    // team). Without the entitlement (an unsigned dev build) UserDefaults(suiteName:)
    // degrades to a plain preferences domain of the same name — which still round-trips
    // between two processes on ONE machine, so the dev/harness path works unchanged.
    public static let suiteName = "group.com.afterdark.modern"

    public static let kAssetsRoot     = "assetsRoot"
    public static let kSharedLibsRoot = "sharedLibsRoot"
    public static let kHostsDir       = "hostsDir"
    public static let kUpdatedAt      = "updatedAt"
    // The global Duration preference rides along on the SAME two channels as the
    // paths. It has to: the saver runs in another process whose defaults domain is
    // its own (ScreenSaverDefaults inside legacyScreenSaver.appex), so it cannot
    // read the app's UserDefaults.standard — but it already reads this sidecar by
    // absolute path, and that is the channel proven to work in the real appex.
    // Carried as a STRING to keep the sidecar's [String: String] shape.
    public static let kDurationSeconds = "durationSeconds"

    // Fixed absolute path (under the app-support dir, i.e. the parent of assetsRoot)
    // of a plain-JSON handoff the SANDBOXED saver reads directly. This is the robust
    // channel: codesign cannot attach an application-groups entitlement to an
    // MH_BUNDLE (a .saver), so inside legacyScreenSaver.appex the app-group suite
    // resolves to the WRONG (appex) container. The saver instead computes the real
    // user home via getpwuid() and reads THIS file through the appex's whole-
    // filesystem READ exception — no entitlement required.
    public static func sidecarPath() -> String {
        let dir = (ADPaths.assetsRoot as NSString).deletingLastPathComponent
        return dir + "/saver-handoff.json"
    }

    // Write the current resolved roots to BOTH the app-group suite (works when the
    // reader carries the entitlement / shares the home) and the JSON sidecar (works
    // in the real appex). Idempotent; safe to call repeatedly (on launch when assets
    // are already present, and after a download finishes).
    public static func publish() {
        // Read straight from defaults rather than taking a parameter, so every
        // existing publish() call site keeps the saver's copy current for free.
        let duration = ADDuration.sanitize(UserDefaults.standard.object(forKey: ADDuration.defaultsKey) as? Int)
        if let d = UserDefaults(suiteName: suiteName) {
            d.set(ADPaths.assetsRoot,     forKey: kAssetsRoot)
            d.set(ADPaths.sharedLibsRoot, forKey: kSharedLibsRoot)
            d.set(ADPaths.hostsDir,       forKey: kHostsDir)
            d.set(duration,               forKey: kDurationSeconds)
            d.set(Date().timeIntervalSince1970, forKey: kUpdatedAt)
        }
        let payload: [String: String] = [
            kAssetsRoot: ADPaths.assetsRoot,
            kSharedLibsRoot: ADPaths.sharedLibsRoot,
            kHostsDir: ADPaths.hostsDir,
            kDurationSeconds: String(duration),
        ]
        if let data = try? JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted]) {
            let path = sidecarPath()
            try? FileManager.default.createDirectory(
                atPath: (path as NSString).deletingLastPathComponent,
                withIntermediateDirectories: true)
            try? data.write(to: URL(fileURLWithPath: path))
        }
    }

    // The saver's read side (exposed here so the contract lives in one file). Returns
    // nil if the app has never published (saver installed but app not yet run).
    public struct Roots { public let assetsRoot: String; public let sharedLibsRoot: String; public let hostsDir: String? }
    public static func read() -> Roots? {
        guard let d = UserDefaults(suiteName: suiteName),
              let assets = d.string(forKey: kAssetsRoot), !assets.isEmpty,
              let shared = d.string(forKey: kSharedLibsRoot), !shared.isEmpty
        else { return nil }
        return Roots(assetsRoot: assets, sharedLibsRoot: shared, hostsDir: d.string(forKey: kHostsDir))
    }
}
