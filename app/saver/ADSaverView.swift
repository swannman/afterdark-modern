import ScreenSaver
import AppKit
import CoreGraphics
import os

// AfterDark.saver principal class. Reuses the app's EXACT emulation drive
// (EmulatedHost: spawn adhost/adhost68k, read the P8/shm frame stream, decode to a
// CGImage) inside the sandboxed macOS screen-saver host. The appex permits
// spawning the host subprocess and reading the copyrighted assets by absolute path.
//
// vs the spike (ADSpikeView): a real ScreenSaverView (not a probe), assets resolved
// from the app-group handoff (no hardcoded paths), a module picker, single-module OR
// cycle-all drive, and an UPRIGHT blit (the spike had a vertical-flip cosmetic bug).
@objc(ADSaverView)
public final class ADSaverView: ScreenSaverView {

    // MARK: Preferences (shared cross-process via ScreenSaverDefaults)
    private static let kSelection = "ADSelectedModule"   // module id, or kCycleAll
    private static let kCycleAll  = "__cycle_all__"

    // The module rotation runs on the user's Duration preference (see ADDuration
    // for the ladder). The preference is authored in the app and reaches this
    // process through the asset handoff, since a saver has no defaults domain in
    // common with the app; ADDuration.defaultSeconds (60 s = the authentic sVal 503
    // factory default) applies until the app has published one.
    private var durationSeconds = ADDuration.defaultSeconds

    private let log = Logger(subsystem: "com.afterdark.saver", category: "view")

    private var modules: [ADModule] = []
    private var haveAssets = false

    // Frame published by the host reader thread; read on the main thread in draw().
    private let frameLock = NSLock()
    private var currentFrame: CGImage?

    private var host: EmulatedHost?
    private var cycleList: [ADModule] = []
    private var cycleIndex = 0
    private var moduleStartedAt = Date()
    private var cycling = false
    private var starting = true          // pre-first-frame

    // The saver does NOT observe Caps-Lock. The host reads the real
    // key itself once per frame under ADREALCAPS (EmulatedHost._buildEnv). That matters
    // most here: a screen-saver appex is not a normal key-window app, so an NSEvent monitor
    // would have been the least dependable part of the chain — and this way there is no
    // per-module state to seed on a cycle.

    // MARK: - Init
    public override init!(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        commonInit()
    }
    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        animationTimeInterval = 1.0 / 30.0
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor

        // Resolve assets from the app-group handoff BEFORE building the catalog
        // (ADPaths' roots are evaluated once, on first access). Bundled hosts live
        // in this .saver's Resources.
        let hostsDir = Bundle(for: ADSaverView.self).resourcePath ?? ""
        let boot = ADSaverBootstrap.installEnv(bundledHostsDir: hostsDir)
        haveAssets = boot.haveAssets
        durationSeconds = ADDuration.sanitize(boot.durationSeconds)
        modules = ADSaverCatalog.load(from: Bundle(for: ADSaverView.self))
        let summary = "init preview=\(isPreview) haveAssets=\(haveAssets) modules=\(modules.count) hosts=\(hostsDir) duration=\(ADDuration.stop(forSeconds: durationSeconds).label)"
        log.info("\(summary, privacy: .public)")
    }

    // MARK: - Selection
    private func defaultsStore() -> UserDefaults? {
        let id = Bundle(for: ADSaverView.self).bundleIdentifier ?? "com.afterdark.saver"
        return ScreenSaverDefaults(forModuleWithName: id)
    }

    // The emulated (non-native, recipe-backed) roster in catalog order.
    private var emulatable: [ADModule] { modules.filter { $0.available && $0.recipe != nil } }

    // Resolve the current selection into either a single module or the cycle list.
    private func resolveSelection() {
        let sel = defaultsStore()?.string(forKey: Self.kSelection) ?? Self.kCycleAll
        if sel == Self.kCycleAll {
            cycling = true
            cycleList = emulatable
        } else if let m = emulatable.first(where: { $0.id == sel }) {
            cycling = false
            cycleList = [m]
        } else {
            // Unknown/native selection -> fall back to cycle-all.
            cycling = true
            cycleList = emulatable
        }
        cycleIndex = 0
    }

    // MARK: - Animation lifecycle
    public override func startAnimation() {
        super.startAnimation()
        guard haveAssets, !emulatable.isEmpty else {
            log.error("no assets / empty roster — showing guidance frame")
            return
        }
        resolveSelection()
        startModule(at: 0)
    }

    public override func stopAnimation() {
        host?.stop()
        host = nil
        frameLock.lock(); currentFrame = nil; frameLock.unlock()
        super.stopAnimation()
    }

    private func startModule(at index: Int) {
        guard !cycleList.isEmpty else { return }
        cycleIndex = index % cycleList.count
        let module = cycleList[cycleIndex]
        host?.stop()
        frameLock.lock(); currentFrame = nil; frameLock.unlock()
        starting = true
        moduleStartedAt = Date()

        // Effective control values: factory defaults for the saver (no per-control
        // UI persistence here — the app owns rich settings; the saver honors module
        // defaults, which EmulatedHost seeds authentically).
        let settings: [String: Int] = [:]
        // ONE Duration boundary = ONE teardown + init of the next pick, exactly
        // like the original control panel: at each boundary the engine tore down and
        // re-inited the next Randomizer pick (with a single enabled module the "next
        // pick" was the same module again). So when this view is rotating across
        // several modules IT owns the boundary and the
        // host's own cycle is disarmed — otherwise both would fire at the same instant
        // and the host would re-init a module we are about to throw away. With a single
        // module there is no rotation and the host performs the authentic same-module
        // re-init itself.
        EmulatedHost.durationSeconds = (cycling && cycleList.count > 1)
            ? ADDuration.forever
            : durationSeconds
        let h = EmulatedHost(module: module, settings: settings) { [weak self] img in
            guard let self else { return }
            self.frameLock.lock()
            self.currentFrame = img
            self.starting = false
            self.frameLock.unlock()
        }
        host = h
        h.start()
        log.info("start module=\(module.id, privacy: .public) cycling=\(self.cycling)")
    }

    public override func animateOneFrame() {
        // Advance the cycle on the wall clock (single-module selections never advance).
        // The period is the user's Duration; "Forever!" means the
        // rotation never comes due at all.
        if cycling, cycleList.count > 1, durationSeconds != ADDuration.forever,
           Date().timeIntervalSince(moduleStartedAt) >= TimeInterval(durationSeconds) {
            startModule(at: cycleIndex + 1)
        }
        setNeedsDisplay(bounds)
    }

    // MARK: - Draw
    public override func draw(_ rect: NSRect) {
        NSColor.black.setFill()
        rect.fill()

        frameLock.lock()
        let img = currentFrame
        let isStarting = starting
        frameLock.unlock()

        if let img {
            // Aspect-fit into bounds with black letterbox bars, nearest-neighbor
            // (crisp pixels). NSImage.draw(respectFlipped:) renders UPRIGHT in this
            // (non-flipped) view — fixing the spike's vertical flip. Retina backing
            // scale is handled by the drawRect context transform.
            let vw = bounds.width, vh = bounds.height
            let iw = CGFloat(img.width), ih = CGFloat(img.height)
            guard iw > 0, ih > 0 else { return }
            let s = min(vw / iw, vh / ih)
            let dw = iw * s, dh = ih * s
            let dst = NSRect(x: (vw - dw) / 2, y: (vh - dh) / 2, width: dw, height: dh)
            let ns = NSImage(cgImage: img, size: NSSize(width: iw, height: ih))
            ns.draw(in: dst, from: .zero, operation: .copy, fraction: 1.0,
                    respectFlipped: true,
                    hints: [.interpolation: NSImageInterpolation.none])
            return
        }

        // No frame yet: guidance (assets missing) or a starting message.
        let msg: String
        if !haveAssets {
            msg = "Open the After Dark app first to download the modules."
        } else if isStarting {
            msg = "Starting After Dark…"
        } else {
            msg = "After Dark"
        }
        drawCenteredMessage(msg, in: rect)
    }

    private func drawCenteredMessage(_ msg: String, in rect: NSRect) {
        let fontSize = max(12, min(rect.height, rect.width) * 0.035)
        let attrs: [NSAttributedString.Key: Any] = [
            .foregroundColor: NSColor(white: 0.85, alpha: 1),
            .font: NSFont.systemFont(ofSize: fontSize)
        ]
        let s = NSAttributedString(string: msg, attributes: attrs)
        let sz = s.size()
        s.draw(at: NSPoint(x: (rect.width - sz.width) / 2, y: (rect.height - sz.height) / 2))
    }

    // MARK: - Configure sheet (module picker)
    public override var hasConfigureSheet: Bool { true }

    private var configController: ADConfigController?
    public override var configureSheet: NSWindow? {
        let ctl = ADConfigController(modules: emulatable,
                                     currentSelection: defaultsStore()?.string(forKey: Self.kSelection) ?? Self.kCycleAll,
                                     cycleAllTag: Self.kCycleAll,
                                     // Read-only here on purpose. The app owns
                                     // the rich settings (same reason there is no per-control UI in
                                     // this sheet), and Duration reaches us from it — so the sheet
                                     // reports the value and says where to change it.
                                     durationLabel: ADDuration.stop(forSeconds: durationSeconds).label) { [weak self] chosen in
            guard let self, let store = self.defaultsStore() else { return }
            store.set(chosen, forKey: Self.kSelection)
            store.synchronize()
            // Apply immediately if animating.
            if self.host != nil {
                self.resolveSelection()
                self.startModule(at: 0)
            }
        }
        configController = ctl        // retain while the sheet is up
        return ctl.window
    }
}

// Builds the module-picker sheet: a popup (Random/cycle-all + every emulated module)
// with Cancel/OK. Returned as an NSWindow so the System Settings host runs it as a
// sheet; OK ends the sheet via NSApp.endSheet (the ScreenSaver configure convention).
final class ADConfigController: NSObject {
    let window: NSWindow
    private let popup: NSPopUpButton
    private let onDone: (String) -> Void
    private let cycleAllTag: String

    init(modules: [ADModule], currentSelection: String, cycleAllTag: String,
         durationLabel: String, onDone: @escaping (String) -> Void) {
        self.onDone = onDone
        self.cycleAllTag = cycleAllTag

        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 176),
                         styleMask: [.titled], backing: .buffered, defer: false)
        w.title = "After Dark"
        self.window = w

        let title = NSTextField(labelWithString: "Screen saver module:")
        title.frame = NSRect(x: 20, y: 134, width: 380, height: 20)

        popup = NSPopUpButton(frame: NSRect(x: 20, y: 100, width: 380, height: 26), pullsDown: false)
        popup.addItem(withTitle: "Random — cycle through all modules")
        popup.lastItem?.representedObject = cycleAllTag
        popup.menu?.addItem(.separator())
        for m in modules {
            let fam = m.family == .ppc ? "PPC" : "68K"
            popup.addItem(withTitle: "\(m.name)  (\(fam))")
            popup.lastItem?.representedObject = m.id
        }
        // Restore the current selection.
        if let idx = popup.itemArray.firstIndex(where: { ($0.representedObject as? String) == currentSelection }) {
            popup.selectItem(at: idx)
        } else {
            popup.selectItem(at: 0)
        }

        super.init()

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        cancel.frame = NSRect(x: 220, y: 16, width: 90, height: 32)
        cancel.bezelStyle = .rounded
        cancel.keyEquivalent = "\u{1b}"

        let ok = NSButton(title: "OK", target: self, action: #selector(ok))
        ok.frame = NSRect(x: 315, y: 16, width: 90, height: 32)
        ok.bezelStyle = .rounded
        ok.keyEquivalent = "\r"

        // Report the Duration the app published, and point at it.
        let dur = NSTextField(labelWithString: "Duration: \(durationLabel)  —  change it in the After Dark app.")
        dur.frame = NSRect(x: 20, y: 62, width: 380, height: 18)
        dur.font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        dur.textColor = .secondaryLabelColor

        let cv = w.contentView!
        cv.addSubview(title); cv.addSubview(popup); cv.addSubview(dur)
        cv.addSubview(cancel); cv.addSubview(ok)
    }

    @objc private func ok() {
        let chosen = (popup.selectedItem?.representedObject as? String) ?? cycleAllTag
        onDone(chosen)
        endSheet()
    }
    @objc private func cancel() { endSheet() }

    private func endSheet() {
        if let parent = window.sheetParent {
            parent.endSheet(window)
        } else {
            NSApp.endSheet(window)
            window.orderOut(nil)
        }
    }
}
