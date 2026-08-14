import Foundation

// Resolves the roots that the catalog's recipe paths are relative to, so the
// project ships NO absolute machine paths. catalog.json holds RELATIVE paths:
//   host                -> resolved to the HOST binary (adhost/adhost68k)
//   workingDir          -> resolved to the SHARED-LIBS dir (the host's CWD)
//   rsrcPath / dataForkPath -> resolved against the assets root (original AD bits)
//
// There are TWO layouts (auto-detected, each env-overridable):
//   DEV: a source checkout. Host binaries + shared libs both live in
//        repoRoot/engine. This is the historical behavior.
//   DISTRIBUTED: a shipped .app. The host binaries are BUNDLED in
//        <App>.app/Contents/Helpers (self-contained: only system dylibs); the
//        COPYRIGHTED shared libs (ADShared40.pef / AD4Library.rsrc) are NEVER
//        bundled — they arrive only via the first-run download and live at
//        assetsRoot/shared. So in distributed mode the host binary comes from the
//        bundle while its CWD (workingDir) is the downloaded shared-libs dir.
//
// The host's CWD matters because the PPC host takes "ADShared40.pef" as a RELATIVE
// argv and reads ADLIB="AD4Library.rsrc" relative to CWD; both must resolve in the
// same dir. (The 68K host derives its shared lib from the module's sibling dir in
// the assets tree, so it is CWD-independent — but it still chdir's there harmlessly.)
//
// An already-absolute path in the catalog is returned unchanged, so a hand-edited
// or legacy absolute entry still works.
public enum ADPaths {
    // Where the fetched original After Dark bits live (populated by tools/adfetch).
    // Override with AD_ASSETS_DIR. Default: the app-support assets dir the fetch
    // tool writes to.
    public static let assetsRoot: String = {
        if let e = env("AD_ASSETS_DIR") { return expand(e) }
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return "\(home)/Library/Application Support/AfterDarkModern/assets"
    }()

    // Where this repo (and its built hosts under engine) lives. Override with
    // AD_REPO_DIR. Default: walk up from the current directory until a dir that
    // contains "engine" is found (handles `swift run` from app/AfterDark);
    // fall back to the current directory.
    public static let repoRoot: String = {
        if let e = env("AD_REPO_DIR") { return expand(e) }
        let fm = FileManager.default
        var dir = fm.currentDirectoryPath
        for _ in 0..<8 {
            if fm.fileExists(atPath: "\(dir)/engine") { return dir }
            let parent = (dir as NSString).deletingLastPathComponent
            if parent == dir { break }
            dir = parent
        }
        return fm.currentDirectoryPath
    }()

    // The bundled host directory (<App>.app/Contents/Helpers) IF at least one host
    // binary is present there — the signal that we're a shipped .app rather than a
    // dev checkout. nil when running from a bare SwiftPM build (swift run / tests),
    // where Bundle.main.bundlePath is just the executable's dir with no Helpers.
    private static let bundledHostsDir: String? = {
        let helpers = "\(Bundle.main.bundlePath)/Contents/Helpers"
        let fm = FileManager.default
        if fm.isExecutableFile(atPath: "\(helpers)/adhost")
            || fm.isExecutableFile(atPath: "\(helpers)/adhost68k") {
            return helpers
        }
        return nil
    }()

    // True when the host binaries come from a bundle/override rather than the dev
    // tree — which also dictates that the shared libs come from the download.
    public static let isDistributed: Bool = (env("AD_HOST_DIR") != nil) || (bundledHostsDir != nil)

    // Directory holding the host binaries (adhost / adhost68k). Override AD_HOST_DIR;
    // else the bundled Helpers dir if present; else the dev tree.
    public static let hostsDir: String = {
        if let e = env("AD_HOST_DIR") { return expand(e) }
        if let b = bundledHostsDir { return b }
        return "\(repoRoot)/engine"
    }()

    // Directory holding the shared runtime libraries (ADShared40.pef, AD4Library.rsrc)
    // — the host's working directory / CWD. Override AD_SHARED_DIR; else in a
    // distributed app the downloaded assetsRoot/shared, else the dev tree.
    public static let sharedLibsRoot: String = {
        if let e = env("AD_SHARED_DIR") { return expand(e) }
        return isDistributed ? "\(assetsRoot)/shared" : "\(repoRoot)/engine"
    }()

    // Resolve a catalog host path to the host binary: takes only its basename
    // (adhost / adhost68k) and joins it under hostsDir, so the catalog's literal
    // "engine/adhost" resolves to the bundle in a shipped app.
    public static func resolveHost(_ path: String) -> String {
        if path.hasPrefix("/") { return path }        // already absolute: pass through
        return "\(hostsDir)/\((path as NSString).lastPathComponent)"
    }

    // Resolve a catalog rsrcPath/dataForkPath against the assets root.
    public static func resolveAsset(_ path: String) -> String { join(assetsRoot, path) }

    // Resolve a catalog host/workingDir path against the repo root. Retained for
    // any callers/overrides that still want a raw repo-relative path.
    public static func resolveRepo(_ path: String) -> String { join(repoRoot, path) }

    private static func join(_ root: String, _ path: String) -> String {
        if path.hasPrefix("/") { return path }        // already absolute: pass through
        return "\(root)/\(path)"
    }
    private static func env(_ k: String) -> String? {
        guard let v = ProcessInfo.processInfo.environment[k], !v.isEmpty else { return nil }
        return v
    }
    private static func expand(_ p: String) -> String {
        (p as NSString).expandingTildeInPath
    }
}
