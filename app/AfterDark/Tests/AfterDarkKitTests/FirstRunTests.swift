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
}
