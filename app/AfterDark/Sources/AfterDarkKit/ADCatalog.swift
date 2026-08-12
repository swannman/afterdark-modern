import Foundation

// Loads catalog.json (bundled resource) into the ADModule roster. The catalog is
// generated from the working-module census + extracted control resources.
public final class ADCatalog {
    public static let shared = ADCatalog()
    public let modules: [ADModule]

    private struct JControl: Decodable {
        let id: String
        let resourceId: Int?
        let type: String
        let label: String
        let defaultValue: Int
        let min: Int?
        let max: Int?
        let options: [String]?
        let valueNames: [String]?
    }
    private struct JModule: Decodable {
        let id: String
        let displayName: String
        let family: String
        let native: Bool?
        let host: String?
        let workingDir: String?
        let env: [String: String]?
        let argsTemplate: [String]?
        let rsrcPath: String?
        let dataForkPath: String?
        let lane: String?
        let controlEnvVar: String?
        let controls: [JControl]
        let about: String?
        let credits: String?
    }
    private struct JCatalog: Decodable { let modules: [JModule] }

    private init() {
        guard let url = Bundle.module.url(forResource: "catalog", withExtension: "json",
                                          subdirectory: "Resources")
                ?? Bundle.module.url(forResource: "catalog", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let cat = try? JSONDecoder().decode(JCatalog.self, from: data) else {
            modules = []
            return
        }
        modules = cat.modules.map { jm in
            let fam: ADFamily = (jm.family == "68K") ? .k68 : .ppc
            let native = jm.native ?? false
            // A module can legitimately expose two controls that share a resource
            // ID across DIFFERENT resource types — e.g. Swirling Magic has both an
            // 'mVal' 1002 (Palette popup) and an 'xVal' 1002 (Magnify Pixies
            // checkbox); in the classic Mac resource model those are distinct
            // resources keyed by (type, id). The catalog derives each control's
            // `id` from its resource ID ("c1002"), so two same-ID controls collide.
            // A duplicate ADControl.id aliases them in BOTH SwiftUI's ForEach
            // identity AND the ADSettingsStore key ("<module>.<control.id>") — the
            // reported bug where selecting one popup mutates the other. Guarantee a
            // unique id per control (suffixing "#2", "#3", … on collision, in stable
            // catalog order) so the inspector renders and persists each control
            // independently. This does NOT touch `resourceId`, which still drives the
            // host control-value index, so the emulation contract is unchanged.
            var seenIds: [String: Int] = [:]
            let controls: [ADControl] = jm.controls.map { c in
                let kind: ADControl.Kind
                switch c.type {
                case "toggle": kind = .toggle
                case "popup":  kind = .popup(options: c.options ?? [])
                case "button": kind = .button
                default:       kind = .slider(min: c.min ?? 0, max: c.max ?? 100)
                }
                let n = (seenIds[c.id] ?? 0) + 1
                seenIds[c.id] = n
                let uniqueId = n == 1 ? c.id : "\(c.id)#\(n)"
                return ADControl(id: uniqueId, label: c.label, kind: kind,
                                 defaultValue: c.defaultValue,
                                 resourceId: c.resourceId ?? 0,
                                 valueNames: c.valueNames)
            }
            var recipe: ADRecipe? = nil
            if !native, let host = jm.host, jm.workingDir != nil,
               let rsrc = jm.rsrcPath, let args = jm.argsTemplate {
                // Catalog paths are relative. The host binary resolves to hostsDir
                // (dev tree OR bundled Helpers); the workingDir/CWD resolves to the
                // SHARED-LIBS dir (dev tree OR the downloaded assets/shared) — NOT
                // literally to the catalog's "tools/adhost", which no longer exists
                // in a shipped app. rsrc/dataFork resolve against the assets root
                // (fetched original bits). See ADPaths.
                recipe = ADRecipe(host: ADPaths.resolveHost(host),
                                  workingDir: ADPaths.sharedLibsRoot,
                                  env: jm.env ?? [:], argsTemplate: args,
                                  rsrcPath: ADPaths.resolveAsset(rsrc),
                                  dataForkPath: jm.dataForkPath.map(ADPaths.resolveAsset),
                                  lane: jm.lane,
                                  controlEnvVar: jm.controlEnvVar ?? "ADCVSET")
            }
            return ADModule(id: jm.id, name: jm.displayName,
                            available: true, family: fam, native: native,
                            recipe: recipe, controls: controls,
                            about: jm.about, credits: jm.credits)
        }
    }
}
