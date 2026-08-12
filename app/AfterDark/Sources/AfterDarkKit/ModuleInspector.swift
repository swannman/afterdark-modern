import SwiftUI

// Right-hand settings panel: the module's own controls up top, its original
// About text below — mirroring the classic After Dark control panel.
public struct ModuleInspector: View {
    public let module: ADModule
    @ObservedObject public var settings: ADSettingsStore

    public init(module: ADModule, settings: ADSettingsStore) {
        self.module = module
        self.settings = settings
    }

    public var body: some View {
        ScrollView {
            ModuleInspectorContent(module: module, settings: settings)
        }
    }
}

// The inspector's content, sans ScrollView (also used for headless snapshots,
// where ScrollView does not lay out under ImageRenderer).
public struct ModuleInspectorContent: View {
    public let module: ADModule
    @ObservedObject public var settings: ADSettingsStore

    public init(module: ADModule, settings: ADSettingsStore) {
        self.module = module
        self.settings = settings
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Settings").font(.headline)

            if module.controls.isEmpty {
                Text(module.available ? "This module has no settings."
                                      : "Settings will appear when this module is ported.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(module.controls) { control in
                    ControlRow(module: module, control: control, settings: settings)
                }
            }

            if module.about != nil || module.credits != nil {
                Divider().padding(.vertical, 4)
                Text("About").font(.headline)
                if let about = module.about {
                    Text(about)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let credits = module.credits, credits != module.about {
                    Text(credits)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            Spacer()
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct ControlRow: View {
    let module: ADModule
    let control: ADControl
    @ObservedObject var settings: ADSettingsStore

    private var value: Binding<Int> {
        Binding(get: { settings.value(for: module, control: control) },
                set: { settings.set($0, for: module, control: control) })
    }

    var body: some View {
        switch control.kind {
        case .slider(let min, let max):
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 4) {
                    // Original labels often already end in ":" — don't double it.
                    Text(control.label.hasSuffix(":") ? control.label : control.label + ":")
                    Text(control.displayValue(value.wrappedValue))
                        .foregroundStyle(.secondary)
                }
                .font(.callout)
                Slider(value: Binding(get: { Double(value.wrappedValue) },
                                      set: { value.wrappedValue = Int($0.rounded()) }),
                       in: Double(min)...Double(max),
                       step: 1)
            }
        case .toggle:
            Toggle(control.label, isOn: Binding(get: { value.wrappedValue != 0 },
                                                set: { value.wrappedValue = $0 ? 1 : 0 }))
                .font(.callout)
                .toggleStyle(.switch)
                .controlSize(.small)
        case .popup(let options):
            Picker(control.label, selection: value) {
                // Mac menu items are 1-BASED; the extracted mVal defaults and the
                // ADCTRL/ADCVSET values injected into the emulation hosts use that
                // numbering, so tag options 1-based (separators keep their slot).
                ForEach(Array(options.enumerated()), id: \.offset) { i, name in
                    if name == "-" {
                        Divider().tag(i + 1)
                    } else {
                        Text(name).tag(i + 1)
                    }
                }
            }
            .font(.callout)
        case .button:
            // Original opened a dialog (e.g. a file/folder picker); inert here.
            HStack {
                Text(control.label).font(.callout)
                Spacer()
                Text("—").foregroundStyle(.tertiary)
            }
        }
    }
}
