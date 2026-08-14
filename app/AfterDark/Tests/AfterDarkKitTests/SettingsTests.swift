import XCTest
@testable import AfterDarkKit

// Pins the settings pipeline to the modules' REAL resource-fork bytes:
//   - control ids are unique per module (the Swirling Magic mVal/xVal 1002
//     dropdown-aliasing bug),
//   - the catalog factory defaults equal the values extracted straight from each
//     module's sVal/mVal/xVal/bVal resources with resource_dasm,
//   - Mac popup menu values are 1-based.
// The expected values below were dumped independently from the module resource
// forks (see LIB510_STATE.md Addendum 614).
final class SettingsTests: XCTestCase {

    private func module(_ id: String) -> ADModule {
        guard let m = ADCatalog.shared.modules.first(where: { $0.id == id }) else {
            fatalError("module \(id) missing from catalog")
        }
        return m
    }

    // MARK: - Dropdown bug: unique ids for two controls sharing a resource ID

    func testSwirlingMagicControlIdsAreUnique() {
        let m = module("ppc-swirling-magic")
        XCTAssertEqual(m.controls.count, 5)
        let ids = m.controls.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count, "control ids must be unique; got \(ids)")

        // Both resource-ID-1002 controls survive as distinct controls.
        let r1002 = m.controls.filter { $0.resourceId == 1002 }
        XCTAssertEqual(r1002.count, 2, "Palette (mVal 1002) + Magnify Pixies (xVal 1002)")
        XCTAssertNotEqual(r1002[0].id, r1002[1].id, "distinct ids so ForEach/settings don't alias")

        // The mVal is a popup, the xVal a toggle — the true widget kinds.
        let palette = m.controls.first { $0.label.hasPrefix("Palette") }!
        let magnify = m.controls.first { $0.label.hasPrefix("Magnify") }!
        if case .popup = palette.kind {} else { XCTFail("Palette should be a popup") }
        if case .toggle = magnify.kind {} else { XCTFail("Magnify Pixies should be a toggle") }
    }

    // MARK: - No settings-store key collision across the two 1002 controls

    func testSwirlingMagicSnapshotHasDistinctKeys() {
        let m = module("ppc-swirling-magic")
        let store = ADSettingsStore()
        // Fresh store => every control resolves to its own factory default under its
        // own key. If the two 1002 controls aliased, the snapshot would drop one.
        let snap = store.snapshot(for: m)
        XCTAssertEqual(snap.count, m.controls.count,
                       "each control must have its own snapshot key")
    }

    // MARK: - Factory defaults == resource-fork bytes (spot-check, both families)

    // (module id, control label prefix, factory default from the resource fork)
    private let factory: [(String, String, Int)] = [
        // PPC Swirling Magic — sVal 1000 / mVal 1001 / mVal 1002 / xVal 1002 / bVal 1003
        ("ppc-swirling-magic", "# of Sea Pixies", 70),
        ("ppc-swirling-magic", "Style",           6),
        ("ppc-swirling-magic", "Palette",         9),
        ("ppc-swirling-magic", "Magnify",         1),
        ("ppc-swirling-magic", "Music",           8),
        // PPC Time Flies
        ("ppc-time-flies", "Type",       4),
        ("ppc-time-flies", "Sounds",     3),
        ("ppc-time-flies", "Drift Speed", 50),
        // 68K Snake
        ("68k-snake", "Solution Speed", 40),
        ("68k-snake", "Maze Complexity", 20),
        ("68k-snake", "Pause when done", 14),
        // 68K Bogglins
        ("68k-bogglins", "Explosiveness", 55),
        ("68k-bogglins", "Twanginess",    47),
        // 68K Clocks
        ("68k-clocks", "Type",          5),
        ("68k-clocks", "Drift Speed",   30),
        ("68k-clocks", "Sounds",        3),
        ("68k-clocks", "Mutation Rate", 90),
    ]

    func testCatalogDefaultsMatchResourceForkBytes() {
        for (mid, label, expected) in factory {
            let m = module(mid)
            guard let c = m.controls.first(where: { $0.label.hasPrefix(label) }) else {
                XCTFail("\(mid): no control labelled \(label)"); continue
            }
            XCTAssertEqual(c.defaultValue, expected,
                           "\(mid) / \(label): catalog default \(c.defaultValue) != factory \(expected)")
        }
    }

    // Mac popup menu values are 1-based: a factory default indexes a real item.
    func testPopupDefaultsAreOneBased() {
        let m = module("ppc-swirling-magic")
        let palette = m.controls.first { $0.label.hasPrefix("Palette") }!
        guard case .popup(let opts) = palette.kind else { return XCTFail() }
        XCTAssertEqual(opts.count, 9)
        XCTAssertEqual(palette.defaultValue, 9)          // 1-based -> item 9
        XCTAssertEqual(opts[palette.defaultValue - 1], "Random")

        let clocks = module("68k-clocks")
        let type = clocks.controls.first { $0.label.hasPrefix("Type") }!
        guard case .popup(let topts) = type.kind else { return XCTFail() }
        XCTAssertEqual(type.defaultValue, 5)
        XCTAssertEqual(topts[type.defaultValue - 1], "Mutating")
    }

    // MARK: - Module ids are unique

    // ADModule is Identifiable/Hashable on `id` alone, so a shared id aliases two
    // modules in SwiftUI's ForEach identity, in `first(where:)` lookups, and in the
    // ADSettingsStore key ("<moduleId>.<controlId>") — the Deluxe "Marbles" /
    // "Marbles!" duplicate-row bug. Same failure mode as the Swirling Magic
    // dropdown collision above, one level up.
    func testCatalogModuleIdsAreUnique() {
        let ids = ADCatalog.shared.modules.map(\.id)
        let dupes = Dictionary(grouping: ids, by: { $0 }).filter { $0.value.count > 1 }.keys
        XCTAssertTrue(dupes.isEmpty, "duplicate module ids: \(Array(dupes))")
    }

    // The two Deluxe Marbles modules are genuinely different programs (1992 Marbles
    // vs 1996 Marbles!), so both stay in the roster with distinct ids and forks.
    func testDeluxeMarblesModulesAreDistinct() {
        let plain = module("68k-marbles"), bang = module("68k-marbles-bang")
        XCTAssertEqual(plain.name, "Marbles")
        XCTAssertEqual(bang.name, "Marbles!")
        XCTAssertNotEqual(plain.recipe?.rsrcPath, bang.recipe?.rsrcPath)
        XCTAssertTrue(plain.recipe!.rsrcPath.hasSuffix("Marbles/..namedfork/rsrc"))
        XCTAssertTrue(bang.recipe!.rsrcPath.hasSuffix("Marbles!/..namedfork/rsrc"))
    }
}

// Pins the Duration ladder to the After Dark 4.0 control panel's
// own resources (AD40_engine.rsrc: sUnt 500/503 labels, rsVl 503 pos->seconds map,
// sVal 503 "Default Duration:" = 0x3C). These are RE'd values, not design choices —
// a diff here means someone edited the ladder, not that a preference moved.
final class DurationTests: XCTestCase {

    func testLadderMatchesControlPanelResources() {
        // rsVl 503, in slider order.
        XCTAssertEqual(ADDuration.stops.map(\.seconds),
                       [15, 30, 60, 120, 300, 600, 1800, 2700, 3600, 5400, 7200, 21600,
                        ADDuration.forever])
        // sUnt 500 (the resource literally named "Duration:"), verbatim — including
        // the inconsistent punctuation on the first two stops.
        XCTAssertEqual(ADDuration.stops.map(\.label),
                       ["15 sec", "30 sec.", "1 min.", "2 min.", "5 min.", "10 min.",
                        "30 min.", "45 min.", "1 hour", "1 h. 30 m.", "2 h.", "6 h.",
                        "Forever!"])
        XCTAssertEqual(ADDuration.label, "Duration:")
    }

    // sVal 503 = 0x3C. This is also the hosts' own ADCYCLESECS default, which is what
    // makes an untouched preference a no-op.
    func testFactoryDefaultIsOneMinute() {
        XCTAssertEqual(ADDuration.defaultSeconds, 60)
        XCTAssertEqual(ADDuration.stop(forSeconds: ADDuration.defaultSeconds).label, "1 min.")
    }

    // A hand-edited defaults plist or a stale sidecar must not put the host on a
    // period the real control never offered.
    func testSanitizeRejectsOffLadderValues() {
        XCTAssertEqual(ADDuration.sanitize(nil), 60)
        XCTAssertEqual(ADDuration.sanitize(45), 60)      // our old, inauthentic cadence
        XCTAssertEqual(ADDuration.sanitize(0), 60)
        XCTAssertEqual(ADDuration.sanitize(1800), 1800)
        XCTAssertEqual(ADDuration.sanitize(ADDuration.forever), ADDuration.forever)
    }

    func testForeverIsNotAConfusableSecondCount() {
        XCTAssertLessThan(ADDuration.forever, 0)
        XCTAssertEqual(ADDuration.stop(forSeconds: ADDuration.forever).label, "Forever!")
    }
}
