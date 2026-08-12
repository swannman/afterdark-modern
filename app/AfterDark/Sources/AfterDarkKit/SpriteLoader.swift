import SpriteKit
import AppKit

// Loads decoded After Dark sprite frames (PNG sequences) from the app bundle.
enum SpriteLoader {
    static var cache: [String: [SKTexture]] = [:]
    private static var contentHeights: [String: CGFloat] = [:]

    // Load all frames of a sprite id (e.g. "s22000") for a module folder.
    static func frames(module: String, sprite: String) -> [SKTexture] {
        let key = "\(module)/\(sprite)"
        if let c = cache[key] { return c }
        let pngs = urls(module: module, sprite: sprite)
        let texs: [SKTexture] = pngs.compactMap { url in
            guard let img = NSImage(contentsOf: url) else { return nil }
            let t = SKTexture(image: img)
            t.filteringMode = .nearest        // crisp pixel-art scaling
            return t
        }
        cache[key] = texs
        return texs
    }

    // Height of the tallest drawn artwork across a sprite's frames, in pixels.
    //
    // Every frame is decoded at the module's declared cel box, which is sized to
    // contain the whole tumble sequence — so a single frame's art can occupy far
    // less than the box. Scaling a node by the box would size the flyer by its
    // empty margins and make sprites with wide tumble ranges look tiny next to the
    // rest of the flock; scaling by this instead sizes every flyer by what is
    // actually visible.
    // `frame` limits the measurement to one frame, for a sprite drawn as a still.
    static func contentHeight(module: String, sprite: String, frame: Int? = nil) -> CGFloat {
        let key = "\(module)/\(sprite)/\(frame.map(String.init) ?? "all")"
        if let h = contentHeights[key] { return h }
        var urls = self.urls(module: module, sprite: sprite)
        if let f = frame { urls = f < urls.count ? [urls[f]] : [] }
        var maxSpan: Int = 0
        for url in urls {
            guard let img = NSImage(contentsOf: url),
                  let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil)
            else { continue }
            maxSpan = max(maxSpan, opaqueRowSpan(cg))
        }
        let h = maxSpan > 0 ? CGFloat(maxSpan) : 1
        contentHeights[key] = h
        return h
    }

    static func urls(module: String, sprite: String) -> [URL] {
        guard let dir = Bundle.module.url(forResource: module, withExtension: nil,
                subdirectory: "Resources")?.appendingPathComponent(sprite) else { return [] }
        let files = (try? FileManager.default.contentsOfDirectory(at: dir,
                        includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "png" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    // Rows between the first and last row holding any non-transparent pixel.
    private static func opaqueRowSpan(_ cg: CGImage) -> Int {
        let w = cg.width, h = cg.height
        guard w > 0, h > 0 else { return 0 }
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return 0 }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        var first = -1, last = -1
        for y in 0..<h {
            var any = false
            let row = y * w * 4
            for x in 0..<w where px[row + x * 4 + 3] != 0 { any = true; break }
            if any { if first < 0 { first = y }; last = y }
        }
        return first < 0 ? 0 : last - first + 1
    }
}
