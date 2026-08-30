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
    private static let kCycleAll  = "__cycle_all__"

    // The module rotation runs on the user's Duration preference (see ADDuration
    // for the ladder). The preference is authored in the app and reaches this
    // process through the asset handoff, since a saver has no defaults domain in
    // common with the app; ADDuration.defaultSeconds (60 s = the authentic sVal 503
    // factory default) applies until the app has published one.
    private var durationSeconds = ADDuration.defaultSeconds
    private var sidecarDuration: Int?
    // The shared settings document (Duration + per-module control values); editable
    // right here in the configure sheet, shared bidirectionally with the app.
    private let saverSettings = ADSaverSettings()

    private let log = Logger(subsystem: "com.swannman.afterdark.saver", category: "view")

    private var modules: [ADModule] = []
    private var haveAssets = false

    // Frames are displayed by assigning CGImages directly to a dedicated layer:
    // one GPU-composited contents swap per frame, no per-frame NSImage or
    // draw(_:) invocation. (The NSImage-per-draw path leaked the appex to
    // gigabytes within minutes at 30fps — CoreGraphics caches per unique image.)
    private let frameLayer = CALayer()
    private let frameLock = NSLock()
    private var haveFrame = false
    // Present by assigning the CGImage directly as the frame layer's contents —
    // the one path field-proven to render inside the sandboxed appex.
    // (IOSurface creation fails silently under the appex sandbox; per-frame
    // NSImage draws leak via CG's image cache. The historic ~10-25MB/s leak was
    // never presentation at all: it was the undrained autorelease pool in the
    // stream reader, fixed in EmulatedHost.)
    // Latest-wins frame presentation: the host emits 30fps but a locked-screen
    // appex may drain the main queue far slower. Naive per-frame async blocks
    // queue unboundedly, each retaining its CGImage (a ~10MB/s leak in the
    // field). At most ONE presentation block is ever in flight; newer frames
    // replace pendingFrame instead of enqueueing.
    private var pendingFrame: CGImage?

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
        frameLayer.contentsGravity = .resizeAspect
        frameLayer.magnificationFilter = .nearest   // crisp pixels, like the old interpolation .none
        frameLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(frameLayer)

        // Resolve assets from the app-group handoff BEFORE building the catalog
        // (ADPaths' roots are evaluated once, on first access). Bundled hosts live
        // in this .saver's Resources.
        let hostsDir = Bundle(for: ADSaverView.self).resourcePath ?? ""
        let boot = ADSaverBootstrap.installEnv(bundledHostsDir: hostsDir)
        haveAssets = boot.haveAssets
        sidecarDuration = boot.durationSeconds
        durationSeconds = saverSettings.durationSeconds(sidecarFallback: sidecarDuration)
        modules = ADSaverCatalog.load(from: Bundle(for: ADSaverView.self))
        let summary = "init preview=\(isPreview) haveAssets=\(haveAssets) modules=\(modules.count) hosts=\(hostsDir) duration=\(ADDuration.stop(forSeconds: durationSeconds).label)"
        log.info("\(summary, privacy: .public)")
    }

    public override func layout() {
        super.layout()
        CATransaction.begin(); CATransaction.setDisableActions(true)
        frameLayer.frame = bounds
        CATransaction.commit()
    }

    private func present(_ img: CGImage) {
        CATransaction.begin(); CATransaction.setDisableActions(true)
        frameLayer.contents = img
        CATransaction.commit()
    }

    // MARK: - Selection
    // The recipe-backed roster in catalog order.
    private var emulatable: [ADModule] { modules.filter { $0.available && $0.recipe != nil } }

    // Resolve the current selection into either a single module or the cycle list.
    // The selection lives in the SHARED settings document, not ScreenSaverDefaults:
    // the Options sheet and the running saver can be different processes whose
    // preference domains never meet (the write literally vanishes into the sheet
    // host's sandbox), while the document is one absolute path every process shares.
    private func resolveSelection() {
        let sel = saverSettings.selection ?? Self.kCycleAll
        if sel == Self.kCycleAll {
            cycling = true
            cycleList = rotationPool().shuffled()
        } else if let m = emulatable.first(where: { $0.id == sel }) {
            cycling = false
            cycleList = [m]
        } else {
            cycling = true
            cycleList = rotationPool().shuffled()
        }
        cycleIndex = 0
    }

    // The Randomizer's participating modules: the checked subset from the
    // Options sheet, or every module when none is stored (the original
    // "All Displays" behavior). Shuffled per pass; every member shows once
    // before any repeats.
    private func rotationPool() -> [ADModule] {
        if let ids = saverSettings.randomizerSet, !ids.isEmpty {
            let pool = emulatable.filter { ids.contains($0.id) }
            if !pool.isEmpty { return pool }
        }
        return emulatable
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
        frameLock.lock(); haveFrame = false; pendingFrame = nil; frameLock.unlock()
        DispatchQueue.main.async { self.frameLayer.contents = nil }
        super.stopAnimation()
    }

    private func startModule(at index: Int) {
        guard !cycleList.isEmpty else { return }
        if cycling, cycleList.count > 1, index >= cycleList.count {
            // Full pass complete: reshuffle so order varies, but avoid an
            // immediate repeat across the boundary.
            let last = cycleList[cycleIndex].id
            repeat { cycleList.shuffle() } while cycleList.count > 1 && cycleList[0].id == last
        }
        cycleIndex = index % cycleList.count
        let module = cycleList[cycleIndex]
        host?.stop()
        frameLock.lock(); haveFrame = false; pendingFrame = nil; frameLock.unlock()
        DispatchQueue.main.async { self.frameLayer.contents = nil }
        starting = true
        moduleStartedAt = Date()

        // Effective control values: the shared settings document (edited in this
        // sheet or in the app — newer edit wins); unset controls run at their
        // factory defaults, which EmulatedHost seeds authentically.
        let settings = saverSettings.snapshot(for: module)
        // Duration is the RANDOMIZER's switching interval (sUnt 500: "the
        // per-Randomizer Duration"; the control panel's sVal 503 is its default).
        // The original never restarted a single selected module — it ran until
        // dismissed. So the host's own cycle is ALWAYS disarmed; when Randomize
        // is on, this view owns the rotation on the wall clock.
        EmulatedHost.durationSeconds = ADDuration.forever
        let h = EmulatedHost(module: module, settings: settings) { [weak self] img in
            guard let self else { return }
            // Store only; presentation happens on the animateOneFrame heartbeat
            // (guaranteed main-thread cadence), which coalesces naturally and has
            // no schedulable state that can wedge.
            self.frameLock.lock()
            self.haveFrame = true
            self.starting = false
            self.pendingFrame = img
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
        pollSharedSettings()
        frameLock.lock()
        let frame = pendingFrame
        pendingFrame = nil
        frameLock.unlock()
        if let frame { present(frame) }
        setNeedsDisplay(bounds)
    }

    // The Options sheet may run in ANOTHER process (System Settings hosts it there),
    // so a running saver learns about edits only from the shared document. Stat the
    // two document paths every couple of seconds; on any mtime change, re-read and
    // apply: selection/duration changes re-resolve the rotation, control-value
    // changes reach the live host (PPC applies in place; 68K respawns, exactly like
    // the app's inspector).
    private var lastSettingsPoll = Date.distantPast
    private var lastSettingsMTime: Date = .distantPast
    private func pollSharedSettings() {
        guard host != nil, Date().timeIntervalSince(lastSettingsPoll) >= 2 else { return }
        lastSettingsPoll = Date()
        let fm = FileManager.default
        var newest = Date.distantPast
        for p in [ADSharedSettings.primaryPath(), ADSharedSettings.mirrorPath()].compactMap({ $0 }) {
            if let m = (try? fm.attributesOfItem(atPath: p))?[.modificationDate] as? Date, m > newest {
                newest = m
            }
        }
        guard newest > lastSettingsMTime else { return }
        let firstPoll = lastSettingsMTime == .distantPast
        lastSettingsMTime = newest
        if firstPoll { return }   // baseline only — don't restart on the first sight

        let oldSelection = saverSettings.selection ?? Self.kCycleAll
        let oldSet = saverSettings.randomizerSet
        let currentModule = cycleList.indices.contains(cycleIndex) ? cycleList[cycleIndex] : nil
        let oldValues = currentModule.map { saverSettings.snapshot(for: $0) }
        saverSettings.reload()
        durationSeconds = saverSettings.durationSeconds(sidecarFallback: sidecarDuration)

        let newSelection = saverSettings.selection ?? Self.kCycleAll
        if cycling, saverSettings.randomizerSet != oldSet {
            log.info("randomizer set changed")
            resolveSelection()
            startModule(at: 0)
            return
        }
        if newSelection != oldSelection {
            log.info("selection changed -> \(newSelection, privacy: .public)")
            resolveSelection()
            startModule(at: 0)
            return
        }
        // Hosts never self-cycle (Duration paces only the Randomize rotation).
        EmulatedHost.durationSeconds = ADDuration.forever
        if let m = currentModule, let old = oldValues {
            let new = saverSettings.snapshot(for: m)
            if new != old { host?.updateSettings(new) }
        }
    }

    // MARK: - Draw
    public override func draw(_ rect: NSRect) {
        NSColor.black.setFill()
        rect.fill()

        frameLock.lock()
        let showingFrames = haveFrame
        let isStarting = starting
        frameLock.unlock()
        if showingFrames { return }   // the frame layer carries the pixels

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

    // MARK: - Configure sheet (the control panel)
    public override var hasConfigureSheet: Bool { true }

    // ONE controller (and window) per view, reused across opens — System Settings'
    // sheet host wedges when a saver hands it a fresh window each time. The
    // controller refreshes its content from the document on every open.
    private var configController: ADConfigController?
    public override var configureSheet: NSWindow? {
        saverSettings.reload()   // absorb edits from the app / another process
        let ctl = configController ?? ADConfigController(
            modules: emulatable,
            settings: saverSettings,
            cycleAllTag: Self.kCycleAll
        ) { [weak self] chosen, duration in
            guard let self else { return }
            // OK already persisted the document; apply to THIS instance if it is
            // the one animating (when the sheet runs in the saver's own process).
            self.durationSeconds = duration
            self.saverSettings.reload()
            if self.host != nil {
                self.resolveSelection()
                self.startModule(at: 0)
            }
        }
        configController = ctl
        ctl.refresh(currentSelection: saverSettings.selection ?? Self.kCycleAll,
                    currentDuration: saverSettings.durationSeconds(sidecarFallback: sidecarDuration))
        return ctl.window
    }
}

// The configure sheet, shaped like the original After Dark control panel: the
// module list on the left, the selected module's REAL controls (parsed from the
// module's own resources, same as the app) on the right, the Duration ladder at
// the bottom. All edits land in the shared settings document, so the app sees
// them too — and vice versa.
final class ADConfigController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let window: NSWindow

    private let modules: [ADModule]
    private let settings: ADSaverSettings
    private let cycleAllTag: String
    private let onDone: (String, Int) -> Void

    private let table = NSTableView()
    // Which modules participate in the Randomize rotation (the checkbox column).
    private var includeSet = Set<String>()
    private let randomize = NSButton(checkboxWithTitle: "Randomize checked", target: nil, action: nil)
    private let durationPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let moduleTitle = NSTextField(labelWithString: "")
    private let controlsStack = NSStackView()
    private let aboutText = NSTextField(wrappingLabelWithString: "")
    private var rows: [ADControlRow] = []

    init(modules: [ADModule], settings: ADSaverSettings,
         cycleAllTag: String,
         onDone: @escaping (String, Int) -> Void) {
        self.modules = modules
        self.settings = settings
        self.cycleAllTag = cycleAllTag
        self.onDone = onDone

        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 700, height: 460),
                         styleMask: [.titled], backing: .buffered, defer: false)
        w.title = "After Dark"
        self.window = w
        super.init()

        let cv = w.contentView!

        // --- Left: the module list -------------------------------------------
        table.dataSource = self
        table.delegate = self
        table.headerView = nil
        table.rowHeight = 20
        table.style = .inset
        let check = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("on"))
        check.width = 22; check.minWidth = 22; check.maxWidth = 22
        table.addTableColumn(check)
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("m"))
        table.addTableColumn(col)
        let listScroll = NSScrollView(frame: NSRect(x: 16, y: 92, width: 224, height: 320))
        listScroll.documentView = table
        listScroll.hasVerticalScroller = true
        listScroll.borderType = .bezelBorder
        listScroll.autoresizingMask = [.height]

        randomize.frame = NSRect(x: 16, y: 420, width: 224, height: 20)
        randomize.target = self
        randomize.action = #selector(randomizeToggled)

        // --- Right: the selected module's controls ---------------------------
        moduleTitle.frame = NSRect(x: 256, y: 414, width: 428, height: 22)
        moduleTitle.font = NSFont.boldSystemFont(ofSize: 14)

        controlsStack.orientation = .vertical
        controlsStack.alignment = .leading
        controlsStack.spacing = 10
        controlsStack.edgeInsets = NSEdgeInsets(top: 8, left: 4, bottom: 8, right: 8)
        controlsStack.translatesAutoresizingMaskIntoConstraints = false

        let controlsDoc = NSView()
        controlsDoc.translatesAutoresizingMaskIntoConstraints = false
        controlsDoc.addSubview(controlsStack)

        let controlsScroll = NSScrollView(frame: NSRect(x: 252, y: 92, width: 432, height: 316))
        controlsScroll.documentView = controlsDoc
        controlsScroll.hasVerticalScroller = true
        controlsScroll.drawsBackground = false
        NSLayoutConstraint.activate([
            controlsStack.topAnchor.constraint(equalTo: controlsDoc.topAnchor),
            controlsStack.leadingAnchor.constraint(equalTo: controlsDoc.leadingAnchor),
            controlsStack.trailingAnchor.constraint(equalTo: controlsDoc.trailingAnchor),
            controlsStack.bottomAnchor.constraint(lessThanOrEqualTo: controlsDoc.bottomAnchor),
            controlsDoc.widthAnchor.constraint(equalToConstant: 416),
        ])

        // --- Bottom bar: Duration + Defaults + Cancel/OK ----------------------
        let durLabel = NSTextField(labelWithString: "Duration:")
        durLabel.frame = NSRect(x: 16, y: 56, width: 70, height: 20)
        for stop in ADDuration.stops {
            durationPopup.addItem(withTitle: stop.label)
            durationPopup.lastItem?.tag = stop.seconds
        }
        durationPopup.frame = NSRect(x: 86, y: 52, width: 130, height: 26)

        let defaults = NSButton(title: "Use Defaults", target: self, action: #selector(useDefaults))
        defaults.frame = NSRect(x: 252, y: 50, width: 120, height: 32)
        defaults.bezelStyle = .rounded

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        cancel.frame = NSRect(x: 494, y: 12, width: 90, height: 32)
        cancel.bezelStyle = .rounded
        cancel.keyEquivalent = "\u{1b}"

        let ok = NSButton(title: "OK", target: self, action: #selector(ok))
        ok.frame = NSRect(x: 592, y: 12, width: 90, height: 32)
        ok.bezelStyle = .rounded
        ok.keyEquivalent = "\r"

        cv.addSubview(randomize)
        cv.addSubview(listScroll)
        cv.addSubview(moduleTitle)
        cv.addSubview(controlsScroll)
        cv.addSubview(durLabel)
        cv.addSubview(durationPopup)
        cv.addSubview(defaults)
        cv.addSubview(cancel)
        cv.addSubview(ok)

        refresh(currentSelection: cycleAllTag, currentDuration: ADDuration.defaultSeconds)
    }

    // (Re)load the sheet's state from the settings document. Called on every open,
    // so a reused window never shows stale values.
    func refresh(currentSelection: String, currentDuration: Int) {
        durationPopup.selectItem(withTag: currentDuration)
        if durationPopup.selectedItem == nil { durationPopup.selectItem(withTag: ADDuration.defaultSeconds) }
        if let ids = settings.randomizerSet, !ids.isEmpty {
            includeSet = Set(ids)
        } else {
            includeSet = Set(modules.map { $0.id })
        }
        table.reloadData()
        suppressSelectionCallback = true
        if currentSelection == cycleAllTag {
            randomize.state = .on
            table.selectRowIndexes([0], byExtendingSelection: false)
        } else if let idx = modules.firstIndex(where: { $0.id == currentSelection }) {
            randomize.state = .off
            table.selectRowIndexes([idx], byExtendingSelection: false)
            table.scrollRowToVisible(idx)
        } else {
            randomize.state = .on
            table.selectRowIndexes([0], byExtendingSelection: false)
        }
        suppressSelectionCallback = false
        showControls(for: selectedModule())
    }

    // MARK: table
    func numberOfRows(in tableView: NSTableView) -> Int { modules.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let m = modules[row]
        if tableColumn?.identifier.rawValue == "on" {
            let cb = NSButton(checkboxWithTitle: "", target: self, action: #selector(includeToggled(_:)))
            cb.state = includeSet.contains(m.id) ? .on : .off
            cb.tag = row
            return cb
        }
        let cell = NSTableCellView()
        let fam = m.family == .ppc ? "PPC" : "68K"
        let tf = NSTextField(labelWithString: "\(m.name)  (\(fam))")
        tf.font = NSFont.systemFont(ofSize: 12)
        tf.lineBreakMode = .byTruncatingTail
        tf.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(tf)
        cell.textField = tf
        NSLayoutConstraint.activate([
            tf.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2),
            tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -2),
            tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }
    private var suppressSelectionCallback = false
    func tableViewSelectionDidChange(_ notification: Notification) {
        // A deliberate click on a module row means "run THIS module": drop out of
        // Randomize, or the row choice would be silently ignored at OK. (Programmatic
        // selection during refresh() doesn't count.)
        if !suppressSelectionCallback { randomize.state = .off }
        showControls(for: selectedModule())
    }

    @objc private func includeToggled(_ sender: NSButton) {
        let row = sender.tag
        guard row >= 0, row < modules.count else { return }
        if sender.state == .on { includeSet.insert(modules[row].id) }
        else { includeSet.remove(modules[row].id) }
    }

    private func selectedModule() -> ADModule? {
        let r = table.selectedRow
        guard r >= 0, r < modules.count else { return nil }
        return modules[r]
    }

    // MARK: controls panel
    private func showControls(for module: ADModule?) {
        rows.removeAll()
        controlsStack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        guard let m = module else { moduleTitle.stringValue = ""; return }
        moduleTitle.stringValue = m.name
        for c in m.controls {
            let row = ADControlRow(module: m, control: c, settings: settings, width: 400)
            rows.append(row)
            controlsStack.addArrangedSubview(row.view)
        }
        if let about = m.about, !about.isEmpty {
            aboutText.stringValue = about
            let a = NSTextField(wrappingLabelWithString: about)
            a.font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
            a.textColor = .secondaryLabelColor
            a.preferredMaxLayoutWidth = 400
            controlsStack.addArrangedSubview(a)
        }
    }

    // MARK: actions
    @objc private func randomizeToggled() { /* run-mode only; list stays for editing */ }

    @objc private func useDefaults() {
        guard let m = selectedModule() else { return }
        settings.resetModule(m)
        showControls(for: m)
    }

    @objc private func ok() {
        let chosen: String
        if randomize.state == .on {
            chosen = cycleAllTag
        } else if let m = selectedModule() {
            chosen = m.id
        } else {
            chosen = cycleAllTag
        }
        let duration = durationPopup.selectedItem?.tag ?? ADDuration.defaultSeconds
        if duration != settings.doc.durationSeconds { settings.setDuration(duration) }
        let allIds = Set(modules.map { $0.id })
        settings.setRandomizerSet(includeSet == allIds || includeSet.isEmpty ? nil : Array(includeSet).sorted())
        settings.setSelection(chosen)
        settings.save()
        onDone(chosen, duration)
        endSheet()
    }
    @objc private func cancel() {
        settings.reload()   // discard staged edits
        endSheet()
    }

    private func endSheet() {
        // Both dismissal paths AND an explicit orderOut: leaving the window attached
        // is what wedges System Settings' sheet host into never opening it again.
        if let parent = window.sheetParent {
            parent.endSheet(window)
        } else {
            NSApp.endSheet(window)
        }
        window.orderOut(nil)
    }
}

// One control row: builds the AppKit editor for a module control and writes edits
// straight into the (staged) shared settings document. Mirrors the app's rendering
// conventions: slider label shows the live value, popups are 1-BASED with "-"
// separators keeping their slot, buttons are inert (the originals opened dialogs).
final class ADControlRow: NSObject {
    let view = NSView()
    private let module: ADModule
    private let control: ADControl
    private let settings: ADSaverSettings
    private var valueLabel: NSTextField?

    init(module: ADModule, control: ADControl, settings: ADSaverSettings, width: CGFloat) {
        self.module = module
        self.control = control
        self.settings = settings
        super.init()

        let value = settings.value(for: module, control: control)
        view.translatesAutoresizingMaskIntoConstraints = false

        switch control.kind {
        case .slider(let min, let max):
            let label = NSTextField(labelWithString: Self.titled(control.label))
            label.font = NSFont.systemFont(ofSize: 12)
            let vl = NSTextField(labelWithString: displayValue(value))
            vl.font = NSFont.systemFont(ofSize: 12)
            vl.textColor = .secondaryLabelColor
            valueLabel = vl
            let slider = NSSlider(value: Double(value), minValue: Double(min),
                                  maxValue: Double(max), target: self,
                                  action: #selector(sliderChanged(_:)))
            slider.isContinuous = true
            for v in [label, vl, slider] { v.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(v) }
            NSLayoutConstraint.activate([
                label.topAnchor.constraint(equalTo: view.topAnchor),
                label.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                vl.centerYAnchor.constraint(equalTo: label.centerYAnchor),
                vl.leadingAnchor.constraint(equalTo: label.trailingAnchor, constant: 6),
                slider.topAnchor.constraint(equalTo: label.bottomAnchor, constant: 2),
                slider.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                slider.widthAnchor.constraint(equalToConstant: width - 20),
                slider.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
        case .toggle:
            let cb = NSButton(checkboxWithTitle: control.label, target: self,
                              action: #selector(toggleChanged(_:)))
            cb.state = value != 0 ? .on : .off
            cb.font = NSFont.systemFont(ofSize: 12)
            cb.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(cb)
            NSLayoutConstraint.activate([
                cb.topAnchor.constraint(equalTo: view.topAnchor),
                cb.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                cb.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
        case .popup(let options):
            let label = NSTextField(labelWithString: Self.titled(control.label))
            label.font = NSFont.systemFont(ofSize: 12)
            let popup = NSPopUpButton(frame: .zero, pullsDown: false)
            // Mac menu items are 1-BASED (the same numbering ADCTRL/ADCVSET inject
            // into the hosts); separators keep their slot.
            for (i, name) in options.enumerated() {
                if name == "-" {
                    popup.menu?.addItem(.separator())
                    popup.lastItem?.tag = i + 1
                } else {
                    popup.addItem(withTitle: name)
                    popup.lastItem?.tag = i + 1
                }
            }
            popup.target = self
            popup.action = #selector(popupChanged(_:))
            popup.selectItem(withTag: value)
            for v in [label, popup] { v.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(v) }
            NSLayoutConstraint.activate([
                label.topAnchor.constraint(equalTo: view.topAnchor),
                label.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                popup.centerYAnchor.constraint(equalTo: label.centerYAnchor),
                popup.leadingAnchor.constraint(equalTo: label.trailingAnchor, constant: 8),
                popup.widthAnchor.constraint(lessThanOrEqualToConstant: 220),
                popup.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
        case .button:
            let label = NSTextField(labelWithString: "\(control.label)  \u{2014}")
            label.font = NSFont.systemFont(ofSize: 12)
            label.textColor = .tertiaryLabelColor
            label.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(label)
            NSLayoutConstraint.activate([
                label.topAnchor.constraint(equalTo: view.topAnchor),
                label.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                label.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
        }
    }

    private static func titled(_ s: String) -> String { s.hasSuffix(":") ? s : s + ":" }

    private func displayValue(_ v: Int) -> String {
        if let names = control.valueNames, v >= 0, v < names.count { return names[v] }
        if case .popup(let options) = control.kind, v >= 0, v < options.count { return options[v] }
        if case .toggle = control.kind { return v != 0 ? "On" : "Off" }
        return String(v)
    }

    @objc private func sliderChanged(_ sender: NSSlider) {
        let v = Int(sender.doubleValue.rounded())
        settings.set(v, for: module, control: control)
        valueLabel?.stringValue = displayValue(v)
    }
    @objc private func toggleChanged(_ sender: NSButton) {
        settings.set(sender.state == .on ? 1 : 0, for: module, control: control)
    }
    @objc private func popupChanged(_ sender: NSPopUpButton) {
        settings.set(sender.selectedTag(), for: module, control: control)
    }
}
