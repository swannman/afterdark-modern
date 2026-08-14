import SwiftUI
import SpriteKit
import AfterDarkKit

@main
struct AfterDarkApp: App {
    @StateObject private var firstRun = FirstRunManager()

    init() {
        // When launched as a bare SwiftPM executable (swift run AfterDark) there is
        // no app bundle, so macOS does not activate the process — the window opens
        // unfocused behind the terminal with no Dock icon. Claim regular-app status
        // and bring ourselves to the front.
        NSApplication.shared.setActivationPolicy(.regular)
        DispatchQueue.main.async {
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    var body: some Scene {
        WindowGroup {
            Group {
                if case .ready = firstRun.stage {
                    MainWindow()
                } else {
                    FirstRunView(manager: firstRun)
                        .onAppear { firstRun.refresh() }
                }
            }
            // Publish the resolved asset/shared-lib/host paths to the shared app
            // group whenever the tree is ready (present at launch, or just downloaded),
            // so the companion AfterDark.saver can locate the modules. addm 705.
            .onChange(of: firstRun.stage) { _ in
                if case .ready = firstRun.stage { ADGroupHandoff.publish() }
            }
        }
        .windowStyle(.titleBar)
    }
}

// The module browser shown once the asset tree is present.
struct MainWindow: View {
    @State private var selection: ADModule? = AD_MODULES.first
    @State private var showInspector = true
    @State private var search = ""
    @StateObject private var settings = ADSettingsStore()
    @StateObject private var duration = ADDurationStore()   // addm 796

    private var ppc: [ADModule] { AD_MODULES.filter { $0.family == .ppc } }
    private var k68: [ADModule] { AD_MODULES.filter { $0.family == .k68 } }

    private func filtered(_ list: [ADModule]) -> [ADModule] {
        guard !search.isEmpty else { return list }
        return list.filter { $0.name.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
            NavigationSplitView {
                List(selection: $selection) {
                    section(ADFamily.ppc.sectionTitle, filtered(ppc))
                    section(ADFamily.k68.sectionTitle, filtered(k68))
                }
                .navigationTitle("After Dark")
                .frame(minWidth: 230)
                .searchable(text: $search, placement: .sidebar,
                            prompt: "Search modules")
                // addm 796: Duration sat in the MAIN control-panel window
                // alongside the module list, not in a Setup sub-dialog — so it lives
                // under the module list here, not in the per-module inspector.
                .safeAreaInset(edge: .bottom) { DurationBar(duration: duration) }
            } detail: {
                if let m = selection {
                    ModuleDetail(module: m, settings: settings)
                        // addm 796: a new Duration has to reach the host's
                        // environment, which it only reads at spawn — so fold it into
                        // the view identity and let the existing onAppear/onDisappear
                        // lifecycle restart the preview, exactly as switching module does.
                        .id("\(m.id)|\(duration.seconds)")
                        .navigationTitle(m.name)
                        .ignoresSafeArea()
                        .inspector(isPresented: $showInspector) {
                            ModuleInspector(module: m, settings: settings)
                                .inspectorColumnWidth(min: 240, ideal: 280, max: 360)
                        }
                        .toolbar {
                            ToolbarItem(placement: .primaryAction) {
                                Button {
                                    showInspector.toggle()
                                } label: {
                                    Label("Settings", systemImage: "sidebar.trailing")
                                }
                                .help("Show or hide module settings")
                            }
                        }
                } else {
                    Text("Select a module").foregroundStyle(.secondary)
                }
            }
    }

    @ViewBuilder
    private func section(_ title: String, _ mods: [ADModule]) -> some View {
        if !mods.isEmpty {
            Section(title) {
                ForEach(mods) { m in
                    HStack(spacing: 8) {
                        Image(systemName: m.native ? "star.circle.fill" : "play.circle")
                            .foregroundStyle(m.native ? .yellow : .green)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(m.name)
                            Text(m.native ? "Native" : m.family.caption)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                    .tag(m)
                }
            }
        }
    }
}

// addm 796: the control panel's Duration slider. Same shape as the app's
// per-module sVal sliders (ControlRow's .slider case: "Label: value" over a step-1
// Slider) because Duration WAS an sVal-family slider — 13 discrete stops, drawn by
// the "Slider CDEF" (CNTL 129). Positions are stop indices; the seconds behind them
// come from rsVl 503. See ADDuration for the resource provenance.
struct DurationBar: View {
    @ObservedObject var duration: ADDurationStore
    @State private var pos: Double = 0

    // Committing only on mouse-up keeps a drag across the ladder from restarting the
    // preview once per stop; the readout still tracks the thumb live.
    private var draggedStop: ADDuration.Stop {
        ADDuration.stops[min(max(Int(pos.rounded()), 0), ADDuration.stops.count - 1)]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Divider()
            HStack(spacing: 4) {
                Text(ADDuration.label)
                Text(draggedStop.label).foregroundStyle(.secondary)
            }
            .font(.callout)
            Slider(value: $pos, in: 0...Double(ADDuration.stops.count - 1), step: 1) { editing in
                if !editing { duration.seconds = draggedStop.seconds }
            }
            Text(draggedStop.seconds == ADDuration.forever
                 ? "Modules run until you wake the screen."
                 : "How often the module restarts, as in the original control panel.")
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 12)
        .padding(.bottom, 10)
        .background(.bar)
        .onAppear { pos = Double(ADDuration.index(forSeconds: duration.seconds)) }
    }
}

// Chooses the native SpriteKit scene or the emulated host preview for a module.
struct ModuleDetail: View {
    let module: ADModule
    @ObservedObject var settings: ADSettingsStore

    var body: some View {
        if module.native {
            ModuleView(module: module, settings: settings)
        } else if module.recipe != nil {
            EmulatedModuleView(module: module, settings: settings)
        } else {
            ModuleView(module: module, settings: settings)   // placeholder scene
        }
    }
}

struct ModuleView: View {
    let module: ADModule
    @ObservedObject var settings: ADSettingsStore
    @State private var scene: SKScene?

    var body: some View {
        GeometryReader { geo in
            SpriteView(scene: currentScene(size: geo.size),
                       options: [.ignoresSiblingOrder])
                .background(Color.black)
                .onChange(of: settings.values) {
                    // Live-apply new control values to the running scene.
                    (scene as? ADConfigurableScene)?
                        .apply(settings: settings.snapshot(for: module))
                }
        }
    }

    private func currentScene(size: CGSize) -> SKScene {
        if let s = scene { return s }
        let s = makeADScene(for: module, size: size,
                            settings: settings.snapshot(for: module))
        DispatchQueue.main.async { scene = s }
        return s
    }
}
