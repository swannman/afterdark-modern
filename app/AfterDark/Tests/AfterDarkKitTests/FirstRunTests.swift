import XCTest
@testable import AfterDarkKit

// Pins the deterministic pieces of the first-run flow: the machine-readable phase
// labels the UI shows, the PATH-independent tool locator, the line-buffer that
// splits adfetch's stdout stream, and the presence/structure detection against a
// temp assets tree. The download drive itself (Process + archive.org) is exercised
// end-to-end by tools/adfetch/adfetch.sh, not here.
final class FirstRunTests: XCTestCase {

    func testPhaseLabelsCoverEveryEmittedPhase() {
        // Every phase adfetch.sh emits must map to a human string (no raw phase leaks).
        for phase in ["ad9-download", "ad9-extract", "dlx-download", "dlx-extract",
                      "shared", "verify", "done", "error"] {
            let label = FirstRunManager.label(forPhase: phase)
            XCTAssertFalse(label.isEmpty)
            XCTAssertNotEqual(label, phase, "phase \(phase) should map to a friendly label")
        }
        // An unknown phase falls back to the raw name rather than crashing.
        XCTAssertEqual(FirstRunManager.label(forPhase: "mystery"), "mystery")
    }

    func testLocateFindsACommonSystemBinary() {
        // /bin/sh is on every macOS; the locator must find it without relying on
        // the (minimal, GUI-launched) process PATH.
        XCTAssertNotNil(FirstRunManager.locate("sh"))
        XCTAssertNil(FirstRunManager.locate("definitely-not-a-real-tool-xyzzy"))
    }

    func testDisclaimerNamesRightsHolderAndSource() {
        let d = FirstRunManager.disclaimer
        XCTAssertTrue(d.contains("Berkeley Systems"))
        XCTAssertTrue(d.contains("Internet Archive"))
    }

    func testLineBufferSplitsAndKeepsRemainder() {
        let lb = LineBuffer()
        // A partial line is held back until its newline arrives.
        XCTAssertEqual(lb.appendAndDrainLines(Data("PROGRESS phase=verify".utf8)), [])
        let lines = lb.appendAndDrainLines(Data(" pct=88\nPROGRESS phase=done pct=100\npart".utf8))
        XCTAssertEqual(lines, ["PROGRESS phase=verify pct=88", "PROGRESS phase=done pct=100"])
        XCTAssertEqual(lb.appendAndDrainLines(Data("ial\n".utf8)), ["partial"])
    }

    @MainActor
    func testAssetsPresentDetectsSentinelAndStructure() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("adfr-\(UUID().uuidString)")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }

        let mgr = FirstRunManager(assetsRootOverride: root.path)
        XCTAssertFalse(mgr.assetsPresent(), "empty tree is not present")

        // Structural completeness: both module trees exist and are non-empty.
        let ppc = root.appendingPathComponent("extracted/After Dark 9 v1.0 (9:9:03)/After Dark Files/After Dark 4.0")
        let k68 = root.appendingPathComponent("deluxe/extracted/dlx/After Dark Deluxe (4")
        try fm.createDirectory(at: ppc, withIntermediateDirectories: true)
        try fm.createDirectory(at: k68, withIntermediateDirectories: true)
        XCTAssertFalse(mgr.assetsPresent(), "empty module dirs are not present")
        fm.createFile(atPath: ppc.appendingPathComponent("SomeModule").path, contents: Data("x".utf8))
        fm.createFile(atPath: k68.appendingPathComponent("Some Module").path, contents: Data("x".utf8))
        XCTAssertTrue(mgr.assetsPresent(), "populated module trees are present (dev-tree coexistence)")

        // The sentinel alone is a sufficient fast path.
        let bare = fm.temporaryDirectory.appendingPathComponent("adfr-\(UUID().uuidString)")
        try fm.createDirectory(at: bare, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: bare) }
        let mgr2 = FirstRunManager(assetsRootOverride: bare.path)
        XCTAssertFalse(mgr2.assetsPresent())
        fm.createFile(atPath: bare.appendingPathComponent(".adfetch-complete").path, contents: Data("ok".utf8))
        XCTAssertTrue(mgr2.assetsPresent(), "sentinel is a sufficient present marker")
    }

    func testDevCheckoutReadsSharedLibsFromDownloadUnlessInstalledInEngine() throws {
        // A fresh clone has no shared libs in engine/ — the first-run download
        // puts them under assetsRoot/shared, so that is where the PPC host's
        // working directory must point. Only an explicit --install-host-libs
        // (libs really present in engine/) switches a dev checkout back.
        let fm = FileManager.default
        let repo = fm.temporaryDirectory.appendingPathComponent("adrepo-\(UUID().uuidString)")
        let engine = repo.appendingPathComponent("engine")
        try fm.createDirectory(at: engine, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: repo) }
        let assets = "/somewhere/assets"

        XCTAssertEqual(ADPaths.resolveSharedLibsRoot(isDistributed: false, repoRoot: repo.path, assetsRoot: assets),
                       "/somewhere/assets/shared")
        fm.createFile(atPath: engine.appendingPathComponent("ADShared40.pef").path, contents: Data("x".utf8))
        XCTAssertEqual(ADPaths.resolveSharedLibsRoot(isDistributed: false, repoRoot: repo.path, assetsRoot: assets),
                       engine.path)
        // A shipped app never reads the dev tree, whatever is lying around in it.
        XCTAssertEqual(ADPaths.resolveSharedLibsRoot(isDistributed: true, repoRoot: repo.path, assetsRoot: assets),
                       "/somewhere/assets/shared")
    }

    func testEveryCatalogModuleFileIsInTheDownloadManifest() throws {
        // The first-run download only places files that assets.manifest.json
        // lists. A module added to catalog.json without a manifest entry works
        // on the machine it was developed on and is silently missing for
        // everyone who downloads — so the catalog must stay a subset.
        let repoRoot = URL(fileURLWithPath: #filePath)   // …/app/AfterDark/Tests/AfterDarkKitTests/X.swift
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        func json(_ rel: String) throws -> [String: Any] {
            let data = try Data(contentsOf: repoRoot.appendingPathComponent(rel))
            return try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any], rel)
        }
        let catalog = try json("app/AfterDark/Sources/AfterDarkKit/Resources/catalog.json")
        let manifest = try json("tools/adfetch/assets.manifest.json")
        let modules = try XCTUnwrap(catalog["modules"] as? [[String: Any]])
        let assets = try XCTUnwrap(manifest["moduleAssets"] as? [[String: Any]])
        let listed = Set(assets.compactMap { $0["path"] as? String })
        XCTAssertFalse(modules.isEmpty)

        var missing: [String] = []
        for m in modules {
            for key in ["rsrcPath", "dataForkPath"] {
                guard let path = m[key] as? String, !path.hasPrefix("/") else { continue }
                if !listed.contains(path) { missing.append("\(m["id"] ?? "?"): \(path)") }
            }
        }
        XCTAssertEqual(missing, [], "catalog modules the download would not fetch — add them to assets.manifest.json")
    }

    func testBundledToolsDirFoundOnlyWhenItHoldsAnExecutableUnar() throws {
        // make_app.sh bundles the extraction tools at Contents/Helpers/bin, and
        // that path is what the downloader gets as AD_TOOLS_DIR — so a bundle
        // without a runnable unar there must report nothing rather than a dir
        // that would silently fall back to whatever is installed.
        let fm = FileManager.default
        let bundle = fm.temporaryDirectory.appendingPathComponent("adfr-\(UUID().uuidString).app")
        let bin = bundle.appendingPathComponent("Contents/Helpers/bin")
        try fm.createDirectory(at: bin, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: bundle) }

        XCTAssertNil(FirstRunManager.toolsDir(inBundleAt: bundle.path), "no unar yet")

        let unar = bin.appendingPathComponent("unar")
        fm.createFile(atPath: unar.path, contents: Data("#!/bin/sh\n".utf8))
        XCTAssertNil(FirstRunManager.toolsDir(inBundleAt: bundle.path), "present but not executable")

        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: unar.path)
        XCTAssertEqual(FirstRunManager.toolsDir(inBundleAt: bundle.path), bin.path)
    }
}
