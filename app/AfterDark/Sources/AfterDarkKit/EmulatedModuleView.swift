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

    // addm 574: modules that react to keyboard input (Caps-Lock interactive mode).
    // Rodger Dodger ("Caps-lock gets you in and out of interactive mode") and Fish
    // World ("Hit Caps-lock to tap on the glass and scare the fish"). Hardcoded for
    // now; these two get keyboard capture + the "interactive" hint caption.
    static let interactiveNames: Set<String> = ["Rodger Dodger", "Fish World"]
    static func isInteractive(_ m: ADModule) -> Bool { interactiveNames.contains(m.name) }

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
            // addm 574: subtle hint that this saver responds to the keyboard.
            if Self.isInteractive(module) && driver.hasFrame {
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

    // addm 574: live input callbacks (wired by EmulatedDriver to the host). onKey
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
    private var lastCaps = false
    private var mouseDownNow = false

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

    // MARK: - addm 574: keyboard + mouse capture (interactive modules only)
    public override var acceptsFirstResponder: Bool { captureInput }

    public override func keyDown(with event: NSEvent) {
        guard captureInput else { super.keyDown(with: event); return }
        onKey?(Int(event.keyCode), true)             // no super => no system beep
    }
    public override func keyUp(with event: NSEvent) {
        guard captureInput else { super.keyUp(with: event); return }
        onKey?(Int(event.keyCode), false)
    }
    public override func flagsChanged(with event: NSEvent) {
        guard captureInput else { super.flagsChanged(with: event); return }
        let caps = event.modifierFlags.contains(.capsLock)
        if caps != lastCaps { lastCaps = caps; onCaps?(caps) }
        super.flagsChanged(with: event)
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
        // addm 574: interactive modules grab keyboard focus so Caps-Lock / keys route
        // here. Deferred so the window has finished setting up its responder chain.
        if captureInput {
            DispatchQueue.main.async { [weak self] in
                guard let self, let win = self.window else { return }
                win.makeFirstResponder(self)
                // Seed the current caps-lock state immediately (so a saver opened with
                // caps already on starts in interactive mode).
                let caps = NSEvent.modifierFlags.contains(.capsLock)
                self.lastCaps = caps; self.onCaps?(caps)
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
        // addm 574: for known-interactive modules, capture keyboard/caps/mouse and
        // forward it to the host over the live input channel.
        if EmulatedModuleView.isInteractive(module) {
            frameView.captureInput = true
            frameView.onKey   = { [weak self] kc, down in self?.host?.sendKey(keycode: kc, down: down) }
            frameView.onCaps  = { [weak self] on in self?.host?.sendCaps(on) }
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
        // addm 569: settings now apply over the live SET channel (no respawn), so
        // the debounce can be short — a moved slider takes effect in ~100ms.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: work)
    }
}
