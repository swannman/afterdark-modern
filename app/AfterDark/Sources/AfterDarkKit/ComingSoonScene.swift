import SpriteKit

// Placeholder for modules not yet ported. Keeps the "pick any module" experience
// working: you can select it and see a titled screen instead of nothing.
final class ComingSoonScene: SKScene {
    private let title: String
    init(size: CGSize, title: String) {
        self.title = title
        super.init(size: size)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func didMove(to view: SKView) { begin() }

    func begin() {
        backgroundColor = SKColor(red: 0.04, green: 0.04, blue: 0.09, alpha: 1)
        scaleMode = .resizeFill
        anchorPoint = .zero
        removeAllChildren()

        let name = SKLabelNode(text: title)
        name.fontName = "Chicago"; name.fontSize = 34
        if name.frame.width == 0 { name.fontName = "Menlo-Bold" }
        name.fontColor = .white
        name.position = CGPoint(x: size.width / 2, y: size.height / 2 + 10)
        name.horizontalAlignmentMode = .center
        addChild(name)

        let sub = SKLabelNode(text: "not yet ported — coming soon")
        sub.fontName = "Menlo"; sub.fontSize = 15
        sub.fontColor = SKColor(white: 0.6, alpha: 1)
        sub.position = CGPoint(x: size.width / 2, y: size.height / 2 - 24)
        addChild(sub)

        // gentle twinkle so it's not fully static
        let star = SKAction.sequence([.fadeAlpha(to: 0.4, duration: 1.2),
                                      .fadeAlpha(to: 1.0, duration: 1.2)])
        name.run(.repeatForever(star))
    }
}
