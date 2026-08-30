import AppKit
import AVFoundation
import ImageIO

// Renders the desktop wallpaper into the P6 PPM the emulation hosts accept as
// ADSEEDIMG, so screen-transformer modules (Down the Drain, Bad Dog!, Spotlight,
// Puzzle, Punch Out, Mowin' Man, ...) operate on the desktop the way they did on
// a real Mac — the screen simply HELD the desktop when the saver kicked in, and
// each module's own DoBlank decided what survived.
//
// This is a wallpaper FILE read, deliberately not a screen capture: the
// ScreenCaptureKit approach was built and cancelled (it triggers the Screen
// Recording permission prompt; see tools/scratchpad/desktopseed_evidence.md).
// A wallpaper is not a photograph of the screen — no windows, no mail — so the
// privacy objection disappears with it.
public enum DesktopSeed {
    // Off by default so adrender/harness/census spawns stay byte-identical;
    // only the saver (and app viewer) opt in.
    public static var enabled = false

    // Cached per (w,h,source identity); the host re-reads the file on every
    // Duration-cycle execv, so the file must outlive individual spawns.
    public static func seedPath(width: Int, height: Int) -> String? {
        guard enabled, width > 0, height > 0 else { return nil }
        guard let src = wallpaperSource() else { return nil }
        let dir = cacheDir()
        let tag = identity(of: src)
        let path = "\(dir)/seed-\(width)x\(height)-\(tag).ppm"
        if FileManager.default.fileExists(atPath: path) { return path }
        guard let img = decode(src) else { return nil }
        guard writePPM(img, width: width, height: height, to: path) else { return nil }
        sweepStale(dir: dir, keep: path)
        return path
    }

    // Source order: the modern Wallpaper store (the user's ACTUAL wallpaper,
    // including aerials via their full-res video), then NSWorkspace (which on
    // modern macOS often reports only DefaultDesktop.heic), then the system
    // default. All are ordinary file reads — no TCC, no prompts.
    private static func wallpaperSource() -> URL? {
        if let u = wallpaperStoreURL() { return u }
        if let screen = NSScreen.main ?? NSScreen.screens.first,
           let u = NSWorkspace.shared.desktopImageURL(for: screen),
           FileManager.default.fileExists(atPath: u.path) { return u }
        let def = URL(fileURLWithPath: "/System/Library/CoreServices/DefaultDesktop.heic")
        return FileManager.default.fileExists(atPath: def.path) ? def : nil
    }

    // macOS 14+ wallpaper config. Best-effort: any parse failure falls through.
    private static func wallpaperStoreURL() -> URL? {
        guard let home = ADSharedSettings.realHome() else { return nil }
        let store = "\(home)/Library/Application Support/com.apple.wallpaper/Store/Index.plist"
        guard let data = FileManager.default.contents(atPath: store),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil),
              let root = plist as? [String: Any] else { return nil }
        // Walk every plausible spaces/display entry; first decodable choice wins.
        var candidates: [[String: Any]] = []
        func collect(_ any: Any?) {
            guard let d = any as? [String: Any] else { return }
            if let desk = d["Desktop"] as? [String: Any] { candidates.append(desk) }
            for v in d.values { collect(v) }
        }
        collect(root)
        for desk in candidates {
            guard let content = desk["Content"] as? [String: Any],
                  let choices = content["Choices"] as? [[String: Any]] else { continue }
            for choice in choices {
                guard let cfgData = choice["Configuration"] as? Data,
                      let cfg = try? PropertyListSerialization.propertyList(from: cfgData, format: nil) else { continue }
                if let u = url(fromConfiguration: cfg, home: home) { return u }
            }
        }
        return nil
    }

    private static func url(fromConfiguration cfg: Any, home: String) -> URL? {
        guard let d = cfg as? [String: Any] else { return nil }
        // Static picture: a file URL (possibly bookmark-relative "Files" entries).
        for key in ["url", "URL"] {
            if let s = d[key] as? String, let u = URL(string: s), u.isFileURL,
               FileManager.default.fileExists(atPath: u.path) { return u }
        }
        if let files = d["Files"] as? [[String: Any]] {
            for f in files {
                if let rel = (f["relative"] as? String) ?? (f["path"] as? String),
                   let u = URL(string: rel), u.isFileURL,
                   FileManager.default.fileExists(atPath: u.path) { return u }
            }
        }
        // Aerial: assetID -> the full-res video; a t=0 frame read is a plain
        // file read, no permission involved.
        if let assetID = d["assetID"] as? String {
            let mov = "\(home)/Library/Application Support/com.apple.idleassetsd/Customer/4KSDR240FPS/\(assetID).mov"
            if FileManager.default.fileExists(atPath: mov) { return URL(fileURLWithPath: mov) }
            let alt = "\(home)/Library/Application Support/com.apple.wallpaper/aerials/videos/\(assetID).mov"
            if FileManager.default.fileExists(atPath: alt) { return URL(fileURLWithPath: alt) }
        }
        return nil
    }

    private static func decode(_ url: URL) -> CGImage? {
        if url.pathExtension.lowercased() == "mov" {
            let gen = AVAssetImageGenerator(asset: AVURLAsset(url: url))
            gen.appliesPreferredTrackTransform = true
            return try? gen.copyCGImage(at: .zero, actualTime: nil)
        }
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    // Aspect-FILL crop (letterboxing would hand Punch Out black bars to punch).
    // NOTE row order: a CGBitmapContext stores row 0 as the TOP row even though
    // its coordinate origin is bottom-left, so drawing into (0,0,w,h) already
    // lands right-side-up — emit rows in natural order, do NOT flip.
    private static func writePPM(_ img: CGImage, width w: Int, height h: Int, to path: String) -> Bool {
        guard let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return false }
        let sx = Double(w) / Double(img.width), sy = Double(h) / Double(img.height)
        let s = max(sx, sy)
        let dw = Double(img.width) * s, dh = Double(img.height) * s
        ctx.interpolationQuality = .high
        ctx.draw(img, in: CGRect(x: (Double(w) - dw) / 2, y: (Double(h) - dh) / 2, width: dw, height: dh))
        guard let buf = ctx.data else { return false }
        var out = Data("P6\n\(w) \(h)\n255\n".utf8)
        out.reserveCapacity(out.count + w * h * 3)
        let px = buf.bindMemory(to: UInt8.self, capacity: w * h * 4)
        for i in 0..<(w * h) {
            out.append(px[i * 4]); out.append(px[i * 4 + 1]); out.append(px[i * 4 + 2])
        }
        return (try? out.write(to: URL(fileURLWithPath: path))) != nil
    }

    private static func cacheDir() -> String {
        // NSHomeDirectory: the appex's own container (writable there, and the
        // host child reads the absolute path just fine); the real home in the app.
        let dir = NSHomeDirectory() + "/Library/Application Support/AfterDarkModern"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    private static func identity(of url: URL) -> String {
        let mtime = ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date)?
            .timeIntervalSince1970 ?? 0
        var h: UInt64 = 0xcbf29ce484222325
        for b in Array("\(url.path)|\(mtime)".utf8) { h = (h ^ UInt64(b)) &* 0x100000001b3 }
        return String(format: "%016llx", h)
    }

    private static func sweepStale(dir: String, keep: String) {
        let fm = FileManager.default
        for f in (try? fm.contentsOfDirectory(atPath: dir)) ?? []
        where f.hasPrefix("seed-") && f.hasSuffix(".ppm") && "\(dir)/\(f)" != keep {
            try? fm.removeItem(atPath: "\(dir)/\(f)")
        }
    }
}
