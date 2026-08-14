import CoreGraphics

public enum ADFamily: String, Hashable {
    case ppc = "PPC"        // After Dark 4.0 (PowerPC)
    case k68 = "68K"        // After Dark Deluxe (68K)
    public var sectionTitle: String {
        switch self {
        case .ppc: return "After Dark 4.0 (PPC)"
        case .k68: return "After Dark Deluxe (68K)"
        }
    }
    public var caption: String { rawValue }
}

// How to launch a module under emulation (built from the catalog).
public struct ADRecipe: Hashable {
    public let host: String            // absolute path to adhost / adhost68k
    public let workingDir: String
    public let env: [String: String]   // static env (before injected frame/control vars)
    public let argsTemplate: [String]  // "{RSRC}" / "{COPY}" placeholders
    public let rsrcPath: String
    public let dataForkPath: String?   // PPC: data fork copied to a temp file for argv
    public let lane: String?           // 68K: "std" | "l40"
    public let controlEnvVar: String   // "ADCVSET" (68K) | "ADCTRL" (PPC)
}

// A screensaver module the app can display.
public struct ADModule: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let available: Bool
    public let family: ADFamily
    public let recipe: ADRecipe?       // launched under emulation via this recipe
    // Module-specific controls shown in the settings inspector, plus the module's
    // original About text and credits line, shown beneath them.
    public let controls: [ADControl]
    public let about: String?
    public let credits: String?
    public init(id: String, name: String, available: Bool,
                family: ADFamily = .ppc,
                recipe: ADRecipe? = nil,
                controls: [ADControl] = [], about: String? = nil,
                credits: String? = nil) {
        self.id = id; self.name = name; self.available = available
        self.family = family; self.recipe = recipe
        self.controls = controls; self.about = about; self.credits = credits
    }
    public static func == (l: ADModule, r: ADModule) -> Bool { l.id == r.id }
    public func hash(into h: inout Hasher) { h.combine(id) }
}

// The full module roster, loaded from the bundled catalog (61 68K + 19 PPC).
public let AD_MODULES: [ADModule] = ADCatalog.shared.modules

