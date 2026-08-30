import Foundation

// Duration — the module-cycle interval — is a USER PREFERENCE, which is what
// it was in the real product.
//
// PROVENANCE (verbatim, not authored). The After Dark 4.0 control panel is the
// top-level "After Dark 4.0" cdev in the AD9 tree (on disk as
// afterdark/ad9_engine/AD40_engine.rsrc). Its main window carries a discrete
// SLIDER (the "Slider CDEF", CNTL 129) whose three defining resources are:
//
//   sUnt 503  slider TICK LABELS, (word pos, pstring) pairs:
//             0 "Short" (left-end caption), then the 13 real stops
//             1 "15 sec. ", 16 "30 sec.", 24 "1 min.", 32 "2 min.", 40 "5 min.",
//             48 "10 min.", 56 "30 min.", 62 "45 min.", 70 "1 hour",
//             78 "1 h. 30 m.", 86 "2 h.", 92 "6 h.", 100 "Forever"
//   rsVl 503  slider pos -> SECONDS: 1->15, 16->30, 24->60, 32->120, 40->300,
//             48->600, 56->1800, 62->2700, 70->3600, 78->5400, 86->7200,
//             92->21600, 100->0xFFFFFFFF ("Forever" = never cycle)
//   sVal 503  resource NAME "Default Duration:", value 0x003C = 60 s
//             => the FACTORY DEFAULT is "1 min.", which is exactly the 60 s the
//             hosts already use for ADCYCLESECS. An untouched preference
//             therefore changes nothing.
//
//   sUnt 500  the SAME ladder for the per-Randomizer Duration in the Randomizer
//             editor; its resource NAME is literally "Duration:" (hence our label)
//             and it differs only in pos 0 ("Default" = inherit the global value)
//             and the exclamation on the top stop ("Forever!"). Its companion
//             mVal 500 / MENU 500 is "Order:" (Random / In Order) — the original
//             paired Order: with Duration:, and Duration existed both globally
//             and per-Randomizer.
//
// Duration was never a module resource — the module corpus has no Duration
// resource anywhere, and the AD 4.0 Faceplate/Shared ship zero sVal resources —
// so there is nothing for a module and the cycle to negotiate; this is purely
// the user's call.
//
// The stop LABELS derive from sUnt 500 (the resource whose name is "Duration:"),
// with two deliberate departures from the original ladder: the punctuation is
// normalized (the panel's "15 sec" / "30 sec." inconsistency is not worth
// preserving in a popup), and the 45 min. stop is replaced by 15 min. — a gap
// the original ladder had that matters for the Randomize rotation.
public enum ADDuration {
    // Our stand-in for the original's 0xFFFFFFFF "Forever!" sentinel. Negative so it
    // can never collide with a real second count.
    public static let forever = -1

    public struct Stop: Hashable {
        public let label: String
        public let seconds: Int      // == ADDuration.forever for "Forever!"
        public init(_ label: String, _ seconds: Int) { self.label = label; self.seconds = seconds }
    }

    // The 13 stops the real slider offered, in slider order (rsVl 503 / sUnt 500).
    public static let stops: [Stop] = [
        Stop("15 sec",      15),
        Stop("30 sec",      30),
        Stop("1 min",       60),
        Stop("2 min",      120),
        Stop("5 min",      300),
        Stop("10 min",     600),
        Stop("15 min",     900),
        Stop("30 min",    1800),
        Stop("1 hour",    3600),
        Stop("1 h 30 m",  5400),
        Stop("2 h",       7200),
        Stop("6 h",      21600),
        Stop("Forever!", forever),
    ]

    /// sVal 503 "Default Duration:" = 0x3C.
    public static let defaultSeconds = 60

    /// The control-panel label, from the sUnt 500 resource name.
    public static let label = "Duration:"

    /// UserDefaults key (app's standard domain; also carried to the saver in the
    /// asset-handoff sidecar under ADGroupHandoff.kDurationSeconds).
    public static let defaultsKey = "ADDurationSeconds"

    /// Index of the stop for a stored value; falls back to the 1 min. default.
    public static func index(forSeconds s: Int) -> Int {
        stops.firstIndex { $0.seconds == s } ?? stops.firstIndex { $0.seconds == defaultSeconds }!
    }

    public static func stop(forSeconds s: Int) -> Stop { stops[index(forSeconds: s)] }

    /// Accept only values that are real slider stops — a hand-edited defaults plist
    /// or a stale sidecar must not put the host on a period the control never offered.
    public static func sanitize(_ s: Int?) -> Int {
        guard let s, stops.contains(where: { $0.seconds == s }) else { return defaultSeconds }
        return s
    }
}
