import XCTest
@testable import AfterDarkKit

final class SharedSettingsTests: XCTestCase {

    func testMergeNewerModuleWins() {
        var a = ADSharedSettings()
        a.set(3, module: "m1", control: "c1", at: 100)
        a.set(7, module: "m2", control: "c1", at: 100)
        var b = ADSharedSettings()
        b.set(5, module: "m1", control: "c1", at: 200)   // newer m1
        let m = ADSharedSettings.merged([a, b])
        XCTAssertEqual(m.value(module: "m1", control: "c1"), 5)
        XCTAssertEqual(m.value(module: "m2", control: "c1"), 7)
    }

    func testMergeReplacesModuleValuesWholesale() {
        // The newer document CLEARED one of the module's controls (Use Defaults);
        // the merge must not resurrect the stale value.
        var a = ADSharedSettings()
        a.set(3, module: "m1", control: "c1", at: 100)
        a.set(4, module: "m1", control: "c2", at: 100)
        var b = ADSharedSettings()
        b.set(9, module: "m1", control: "c1", at: 200)   // c2 intentionally absent
        let m = ADSharedSettings.merged([a, b])
        XCTAssertEqual(m.value(module: "m1", control: "c1"), 9)
        XCTAssertNil(m.value(module: "m1", control: "c2"))
    }

    func testMergeDurationNewerWins() {
        var a = ADSharedSettings(); a.setDuration(60, at: 100)
        var b = ADSharedSettings(); b.setDuration(300, at: 50)
        XCTAssertEqual(ADSharedSettings.merged([a, b]).durationSeconds, 60)
        XCTAssertEqual(ADSharedSettings.merged([b, a]).durationSeconds, 60)
    }

    func testClearModuleRemovesOnlyThatModule() {
        var d = ADSharedSettings()
        d.set(1, module: "m1", control: "c1", at: 100)
        d.set(2, module: "m2", control: "c1", at: 100)
        d.clearModule("m1", at: 200)
        XCTAssertNil(d.value(module: "m1", control: "c1"))
        XCTAssertEqual(d.value(module: "m2", control: "c1"), 2)
        XCTAssertEqual(d.moduleStamps["m1"], 200)
    }

    func testRandomizerSetMergeNewerWins() {
        var a = ADSharedSettings(); a.setRandomizerSet(["m1","m2"], at: 100)
        var b = ADSharedSettings(); b.setRandomizerSet(["m3"], at: 200)
        XCTAssertEqual(ADSharedSettings.merged([a, b]).randomizerSet, ["m3"])
        XCTAssertEqual(ADSharedSettings.merged([b, a]).randomizerSet, ["m3"])
        // clearing back to "all" (nil) must also win by recency
        var c = ADSharedSettings(); c.setRandomizerSet(nil, at: 300)
        XCTAssertNil(ADSharedSettings.merged([b, c]).randomizerSet)
    }

    func testRoundTripThroughDisk() {
        var d = ADSharedSettings()
        d.set(42, module: "m1", control: "c1", at: 123)
        d.setDuration(300, at: 456)
        let path = NSTemporaryDirectory() + "adshared-test-\(UUID().uuidString)/settings.json"
        XCTAssertTrue(ADSharedSettings.write(d, to: path))
        let r = ADSharedSettings.load(path)
        XCTAssertEqual(r?.value(module: "m1", control: "c1"), 42)
        XCTAssertEqual(r?.durationSeconds, 300)
        XCTAssertEqual(r?.durationStamp, 456)
        try? FileManager.default.removeItem(atPath: (path as NSString).deletingLastPathComponent)
    }
}

final class DesktopSeedTests: XCTestCase {
    // End-to-end on the build machine: resolve a wallpaper source, render, and
    // check the exact P6 contract the hosts parse (header + w*h*3 payload).
    // Skips (rather than fails) only if no wallpaper source exists at all.
    func testSeedProducesHostParsablePPM() throws {
        DesktopSeed.enabled = true
        defer { DesktopSeed.enabled = false }
        guard let path = DesktopSeed.seedPath(width: 856, height: 480) else {
            throw XCTSkip("no wallpaper source resolvable in this environment")
        }
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        let header = Data("P6\n856 480\n255\n".utf8)
        XCTAssertEqual(data.prefix(header.count), header)
        XCTAssertEqual(data.count, header.count + 856 * 480 * 3)
        // A real image should show byte variety — but a headless CI runner can
        // resolve a wallpaper yet decode it to a constant fill (no HEIC decode in
        // the VM), and a solid-colour wallpaper is legitimately constant too.
        let variety = Set(data.suffix(from: header.count).prefix(30000)).count
        if variety <= 1 {
            throw XCTSkip("decoder produced a constant fill (headless environment)")
        }
        XCTAssertGreaterThan(variety, 8)
        // Cache hit returns the same file.
        XCTAssertEqual(DesktopSeed.seedPath(width: 856, height: 480), path)
    }
}
