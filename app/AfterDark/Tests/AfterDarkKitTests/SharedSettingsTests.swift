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
