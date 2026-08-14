import SwiftUI
import Combine
import CoreGraphics
import AppKit

// Live preview for an emulated (non-native) module. Runs an EmulatedHost and
// shows the newest decoded frame, presented directly on a CALayer (no per-frame
// SwiftUI view-diff). Settings changes are debounced and applied by respawning
// the host with new control values.
public struct EmulatedModuleView: View {
    public let module: ADModule
    @ObservedObject var settings: ADSettingsStore
    @StateObject private var driver: EmulatedDriver

    public init(module: ADModule, settings: ADSettingsStore) {
        self.module = module
        self.settings = settings
        _driver = StateObject(wrappedValue: EmulatedDriver(module: module,
                                                           settings: settings))
    }

    // Modules that take ARROW/WASD keys and the mouse. These need
    // first-responder capture (they swallow keystrokes), so the set stays small and
    // explicit. Caps-Lock is NOT part of this gate — see capsConsumer below.
    static let interactiveNames: Set<String> = ["Rodger Dodger", "Fish World"]
    static func isInteractive(_ m: ADModule) -> Bool { interactiveNames.contains(m.name) }

    // Does this module document a Caps-Lock behaviour? Derived from
    // the module's OWN About text (shipped in catalog.json), not a hand-kept list, so it
    // can never drift out of sync with the corpus. 19 modules match across both families
    // — Time Flies ("Caps-lock changes the type of clock"), Mandelbrot ("change the color
    // scheme"), Satori, Nirvana, Psycho Deli, Swirling Magic, Magic Turtle, Marbles!,
    // Rock Paper Scissors, Confetti Factory, Strange Attractors, Fish World, Rodger
    // Dodger, in 68K and/or PPC form. Used ONLY to decide whether to show the hint
    // caption; caps itself is forwarded for every module (see EmulatedDriver.init).
    static func capsConsumer(_ m: ADModule) -> Bool {
        guard let about = m.about else { return false }
        return about.range(of: "caps[ \u{2010}-\u{2015}-]?lock",
                           options: [.regularExpression, .caseInsensitive]) != nil
    }

    public var body: some View {
        ZStack {
            // The pane itself is always black — letterbox bars and any area not
            // covered by the streamed frame must never show the window background.
            Color.black.ignoresSafeArea()
            // The CALayer-backed view is always mounted (so occlusion wiring is
            // live); it just stays black until the first frame arrives.
            FrameLayerRepresentable(view: driver.frameView)
            if !driver.hasFrame {
                VStack(spacing: 10) {
                    ProgressView().controlSize(.large)
                    Text("Starting \(module.name)…")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            // Subtle hint that this saver responds to the keyboard: shown for
            // interactive modules and for any module that documents a Caps-Lock
            // behaviour.
            if (Self.capsConsumer(module) || Self.isInteractive(module)) && driver.hasFrame {
                VStack {
                    Spacer()
                    HStack {
                        Text("⌨ interactive · Caps-Lock")
                            .font(.caption2).foregroundStyle(.white.opacity(0.85))
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .background(.black.opacity(0.45), in: Capsule())
                        Spacer()
                    }
                    .padding(10)
                }
                .allowsHitTesting(false)   // never steal clicks from the capture view
            }
        }
        .onAppear { driver.start() }
        .onDisappear { driver.stop() }
        .onChange(of: settings.values) { driver.settingsChanged() }
    }
}

// NSView subclass whose backing layer receives one CGImage per frame via
// `contents` — aspect-fit (black letterbox bars), nearest-neighbor scaling
// (crisp pixels). Also reports window occlusion / app-hide transitions so the
// driver can pause the host (backpressure idles it at ~0% CPU).
public final class FrameLayerNSView: NSView {
    var onVisibility: ((Bool) -> Void)?

    // Live input callbacks (wired by EmulatedDriver to the host). onKey
    // fires for keyDown/keyUp with the Mac virtual keycode (NSEvent.keyCode IS the
    // Mac virtual keycode the host's KeyMap expects). onCaps fires on caps-lock STATE
    // changes (flagsChanged). onMouse fires with FRAME-LOCAL coords (aspect-fit
    // mapped) + button. Set only for known-interactive modules so ordinary savers
    // never grab the keyboard.
    var onKey: ((_ keycode: Int, _ down: Bool) -> Void)?
    var onCaps: ((_ on: Bool) -> Void)?
    var onMouse: ((_ x: Int, _ y: Int, _ button: Bool) -> Void)?
    var captureInput = false                 // gates first-responder + event capture
    private var lastImageSize: CGSize = .zero // native frame size for mouse mapping
    private var mouseDownNow = false

    // This view does not observe Caps-Lock at all. The host reads the real key
    // itself once per frame under ADREALCAPS (EmulatedHost._buildEnv), which needs
    // no first responder, no key window and no event delivery — so it behaves the
    // same in this pane and inside the screen-saver appex, and it survives host respawns
    // and module cycling with no state to replay. An NSEvent monitor + poll here would be
    // a second, weaker source of the same fact.

    public override init(frame: NSRect) { super.init(frame: frame); setup() }
    public required init?(coder: NSCoder) { super.init(coder: coder); setup() }

    private func setup() {
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect      // aspect-fit => letterbox
        layer?.magnificationFilter = .nearest       // crisp pixels
        layer?.minificationFilter = .nearest
    }

    // Set the newest frame with no implicit animation. Main thread only.
    func present(_ img: CGImage) {
        lastImageSize = CGSize(width: img.width, height: img.height)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer?.contents = img
        CATransaction.commit()
    }

    // MARK: - Keyboard + mouse capture (interactive modules only)
    public override var acceptsFirstResponder: Bool { captureInput }

    public override func keyDown(with event: NSEvent) {
        guard captureInput else { super.keyDown(with: event); return }
        onKey?(Int(event.keyCode), true)             // no super => no system beep
    }
    public override func keyUp(with event: NSEvent) {
        guard captureInput else { super.keyUp(with: event); return }
        onKey?(Int(event.keyCode), false)
    }

    // Map a point in view (bottom-left origin) coords to FRAME-local (top-left
    // origin) coords through the aspect-fit letterbox. Returns nil if outside the
    // displayed image rect.
    private func frameCoord(_ p: CGPoint) -> (Int, Int)? {
        let iw = lastImageSize.width, ih = lastImageSize.height
        guard iw > 0, ih > 0 else { return nil }
        let vw = bounds.width, vh = bounds.height
        let scale = min(vw / iw, vh / ih)
        let dw = iw * scale, dh = ih * scale
        let ox = (vw - dw) / 2, oy = (vh - dh) / 2
        let fx = (p.x - ox) / scale
        let fyBottom = (p.y - oy) / scale
        let fy = ih - fyBottom                       // flip to top-left origin
        guard fx >= 0, fx < iw, fy >= 0, fy < ih else { return nil }
        return (Int(fx), Int(fy))
    }
    private func forwardMouse(_ event: NSEvent) {
        guard captureInput else { return }
        let p = convert(event.locationInWindow, from: nil)
        if let (x, y) = frameCoord(p) { onMouse?(x, y, mouseDownNow) }
    }
    public override func mouseMoved(with e: NSEvent) { forwardMouse(e) }
    public override func mouseDragged(with e: NSEvent) { forwardMouse(e) }
    public override func mouseDown(with e: NSEvent) {
        guard captureInput else { super.mouseDown(with: e); return }
        mouseDownNow = true; window?.makeFirstResponder(self); forwardMouse(e)
    }
    public override func mouseUp(with e: NSEvent) {
        guard captureInput else { super.mouseUp(with: e); return }
        mouseDownNow = false; forwardMouse(e)
    }
    public override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for ta in trackingAreas { removeTrackingArea(ta) }
        guard captureInput else { return }
        addTrackingArea(NSTrackingArea(rect: bounds,
            options: [.activeInKeyWindow, .mouseMoved, .inVisibleRect],
            owner: self, userInfo: nil))
    }

    public override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        let nc = NotificationCenter.default
        nc.removeObserver(self)
        guard let win = window else { return }
        nc.addObserver(self, selector: #selector(occlusionChanged),
                       name: NSWindow.didChangeOcclusionStateNotification, object: win)
        nc.addObserver(self, selector: #selector(appHidden),
                       name: NSApplication.didHideNotification, object: nil)
        nc.addObserver(self, selector: #selector(occlusionChanged),
                       name: NSApplication.didUnhideNotification, object: nil)
        updateVisibility()
        // Arrow/WASD + mouse modules grab keyboard focus so those keys route
        // here. Deferred so the window has finished setting up its responder chain.
        if captureInput {
            DispatchQueue.main.async { [weak self] in
                guard let self, let win = self.window else { return }
                win.makeFirstResponder(self)
            }
        }
    }

    @objc private func appHidden() { onVisibility?(false) }
    @objc private func occlusionChanged() { updateVisibility() }

    private func updateVisibility() {
        let visible = (window?.occlusionState.contains(.visible) ?? true) && !NSApp.isHidden
        onVisibility?(visible)
    }

    deinit { NotificationCenter.default.removeObserver(self) }
}

// Bridges the driver-owned FrameLayerNSView into SwiftUI.
struct FrameLayerRepresentable: NSViewRepresentable {
    let view: FrameLayerNSView
    func makeNSView(context: Context) -> FrameLayerNSView { view }
    func updateNSView(_ nsView: FrameLayerNSView, context: Context) {}
}

// Presents one streamed frame aspect-fit in whatever pane size it is given:
// centered, nearest-neighbor scaled (crisp pixels), with black letterbox bars.
// Retained as a pure-SwiftUI renderer so adrender can verify the letterboxing
// geometry headlessly (the live path uses FrameLayerNSView instead).
public struct LetterboxedFrameView: View {
    public let image: CGImage
    public let label: String

    public init(image: CGImage, label: String = "frame") {
        self.image = image
        self.label = label
    }

    public var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            Image(image, scale: 1, label: Text(label))
                .resizable()
                .interpolation(.none)          // nearest-neighbor: crisp pixels
                .aspectRatio(CGFloat(image.width) / CGFloat(max(image.height, 1)),
                             contentMode: .fit) // aspect-fit, ZStack centers it
        }
    }
}

// Owns the EmulatedHost, presents frames onto a CALayer, debounces settings,
// pauses on occlusion/hide, and guarantees teardown.
final class EmulatedDriver: ObservableObject {
    // Flips true once the first frame lands (drives the Starting… overlay).
    @Published var hasFrame = false

    // Driver-owned CALayer view; frames are pushed straight to it (no @Published
    // CGImage => no SwiftUI view-diff per frame).
    let frameView = FrameLayerNSView()

    private let module: ADModule
    private let settings: ADSettingsStore
    private var host: EmulatedHost?
    private var debounce: DispatchWorkItem?
    private var started = false

    init(module: ADModule, settings: ADSettingsStore) {
        self.module = module
        self.settings = settings
        // Occlusion/app-hide -> pause the host (idle via pipe backpressure).
        frameView.onVisibility = { [weak self] visible in
            self?.host?.setPaused(!visible)
        }
        // Arrow/WASD + mouse need first-responder capture, so they stay gated. Caps is
        // NOT here: the host polls the real key itself.
        if EmulatedModuleView.isInteractive(module) {
            frameView.captureInput = true
            frameView.onKey   = { [weak self] kc, down in self?.host?.sendKey(keycode: kc, down: down) }
            frameView.onMouse = { [weak self] x, y, btn in self?.host?.sendMouse(x: x, y: y, button: btn) }
        }
    }

    deinit { host?.stop() }

    func start() {
        guard !started, module.recipe != nil else { return }
        started = true
        let snap = settings.snapshot(for: module)
        let h = EmulatedHost(module: module, settings: snap) { [weak self] img in
            // Reader thread -> main: set the layer contents; no SwiftUI diffing.
            DispatchQueue.main.async {
                guard let self else { return }
                self.frameView.present(img)
                if !self.hasFrame { self.hasFrame = true }
            }
        }
        host = h
        h.start()
    }

    func stop() {
        debounce?.cancel()
        host?.stop()
        host = nil
        started = false
    }

    func settingsChanged() {
        debounce?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, let h = self.host else { return }
            h.updateSettings(self.settings.snapshot(for: self.module))
        }
        debounce = work
        // Settings apply over the live SET channel (no respawn), so
        // the debounce can be short — a moved slider takes effect in ~100ms.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: work)
    }
}
