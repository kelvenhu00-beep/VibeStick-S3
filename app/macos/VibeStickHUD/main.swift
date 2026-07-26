import AppKit
import Foundation
import QuartzCore

private struct HudState: Decodable {
    let active: Bool
    let status: String
    let text: String
    let updated_at_epoch: Double?
    let expires_at_epoch: Double?
}

private final class WaveView: NSView {
    private let bars = (0..<5).map { _ in CALayer() }
    private var animating = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setup()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }

    override func layout() {
        super.layout()
        layoutBars()
    }

    func setActive(_ active: Bool, status: String) {
        if status == "failed" || status == "unclear" {
            stopAnimating()
            return
        }
        active ? startAnimating() : stopAnimating()
    }

    private func setup() {
        wantsLayer = true
        layer?.masksToBounds = false
        for bar in bars {
            bar.backgroundColor = NSColor.white.withAlphaComponent(0.82).cgColor
            bar.cornerRadius = 2.5
            layer?.addSublayer(bar)
        }
        layoutBars()
        startAnimating()
    }

    private func layoutBars() {
        let barWidth: CGFloat = 5
        let spacing: CGFloat = 5
        let heights: [CGFloat] = [14, 24, 34, 24, 14]
        let totalWidth = CGFloat(bars.count) * barWidth + CGFloat(bars.count - 1) * spacing
        let startX = bounds.midX - totalWidth / 2

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for (index, bar) in bars.enumerated() {
            let height = heights[index]
            let x = startX + CGFloat(index) * (barWidth + spacing)
            bar.frame = CGRect(
                x: x,
                y: bounds.midY - height / 2,
                width: barWidth,
                height: height
            )
            bar.anchorPoint = CGPoint(x: 0.5, y: 0.5)
        }
        CATransaction.commit()
    }

    private func startAnimating() {
        guard !animating else { return }
        animating = true
        for (index, bar) in bars.enumerated() {
            bar.opacity = 1
            let animation = CABasicAnimation(keyPath: "transform.scale.y")
            animation.fromValue = 0.35
            animation.toValue = 1.15
            animation.duration = 0.5
            animation.autoreverses = true
            animation.repeatCount = .infinity
            animation.beginTime = CACurrentMediaTime() + Double(index) * 0.08
            animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            bar.add(animation, forKey: "vibestick-wave")
        }
    }

    private func stopAnimating() {
        animating = false
        for bar in bars {
            bar.removeAnimation(forKey: "vibestick-wave")
            bar.opacity = 0.5
            bar.transform = CATransform3DIdentity
        }
    }
}

private final class HudContentView: NSView {
    private let stack = NSStackView()
    private let waveView = WaveView()
    private let textLabel = NSTextField(labelWithString: "")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setup()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }

    func update(text: String, status: String) {
        textLabel.stringValue = text
        waveView.setActive(true, status: status)
        if status == "failed" {
            textLabel.textColor = NSColor.white.withAlphaComponent(0.86)
        } else if status == "unclear" {
            textLabel.textColor = NSColor.white.withAlphaComponent(0.78)
        } else {
            textLabel.textColor = NSColor.white
        }
    }

    private func setup() {
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.withAlphaComponent(0.76).cgColor
        layer?.cornerRadius = 22
        layer?.cornerCurve = .continuous
        layer?.borderWidth = 1
        layer?.borderColor = NSColor.white.withAlphaComponent(0.13).cgColor

        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.distribution = .gravityAreas
        stack.spacing = 14
        stack.translatesAutoresizingMaskIntoConstraints = false

        waveView.translatesAutoresizingMaskIntoConstraints = false
        textLabel.translatesAutoresizingMaskIntoConstraints = false
        textLabel.font = .systemFont(ofSize: 20, weight: .medium)
        textLabel.textColor = .white
        textLabel.alignment = .center
        textLabel.lineBreakMode = .byClipping
        textLabel.setContentHuggingPriority(.required, for: .horizontal)
        textLabel.setContentCompressionResistancePriority(.required, for: .horizontal)

        stack.addArrangedSubview(waveView)
        stack.addArrangedSubview(textLabel)
        addSubview(stack)

        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -24),

            waveView.widthAnchor.constraint(equalToConstant: 48),
            waveView.heightAnchor.constraint(equalToConstant: 42),
        ])
    }
}

private final class HudController {
    private let statePath: URL
    private let window: NSPanel
    private let contentView = HudContentView(frame: NSRect(x: 0, y: 0, width: 252, height: 72))
    private var currentSignature = ""

    init() {
        let support = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VibeStick")
        statePath = support.appendingPathComponent("hud-state.json")

        window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 252, height: 72),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.ignoresMouseEvents = true
        window.level = .screenSaver
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
        window.animationBehavior = .utilityWindow
        window.contentView = contentView
        window.alphaValue = 0

        Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    private func poll() {
        guard let state = readState() else {
            hide()
            return
        }
        if let expires = state.expires_at_epoch, Date().timeIntervalSince1970 >= expires {
            hide()
            return
        }
        guard state.active else {
            hide()
            return
        }
        let text = state.text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            hide()
            return
        }
        show(text: text, status: state.status)
    }

    private func readState() -> HudState? {
        do {
            let data = try Data(contentsOf: statePath)
            return try JSONDecoder().decode(HudState.self, from: data)
        } catch {
            return nil
        }
    }

    private func show(text: String, status: String) {
        let signature = "\(status):\(text)"
        if currentSignature != signature {
            currentSignature = signature
            contentView.update(text: text, status: status)
        }
        reposition(for: text)
        if !window.isVisible {
            window.orderFrontRegardless()
        }
        if window.alphaValue < 1 {
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.12
                window.animator().alphaValue = 1
            }
        }
    }

    private func hide() {
        guard window.isVisible || window.alphaValue > 0 else { return }
        currentSignature = ""
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 0
        } completionHandler: {
            self.window.orderOut(nil)
        }
    }

    private func reposition(for text: String) {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visible = screen.visibleFrame
        let font = NSFont.systemFont(ofSize: 20, weight: .medium)
        let textWidth = ceil((text as NSString).size(withAttributes: [.font: font]).width)
        let contentWidth: CGFloat = 48 + 14 + textWidth
        let width = min(300, max(232, contentWidth + 52))
        let size = NSSize(width: width, height: 72)
        let bottomOffset: CGFloat = 32
        let frame = NSRect(
            x: visible.midX - size.width / 2,
            y: visible.minY + bottomOffset,
            width: size.width,
            height: size.height
        )
        window.setFrame(frame, display: true)
    }
}

private final class AppDelegate: NSObject, NSApplicationDelegate {
    private var hud: HudController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        hud = HudController()
    }
}

private let app = NSApplication.shared
private let delegate = AppDelegate()
app.delegate = delegate
app.run()
