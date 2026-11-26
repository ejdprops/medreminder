#if os(tvOS)
import SwiftUI

/// Apple TV launch splash (7i): dose-colored pills rain top to bottom, then the Did I? mark pulls
/// back into focus at center and the wordmark rises in under it. ~4.5 s; any remote press skips it.
/// Approved preview: claude.ai artifact "Did I? Apple TV splash" (Rack focus). Timings below are
/// in seconds and match it. Layout is authored in a 1920x1080 design space and scaled to the view.
/// Art: SplashLogo (the icon without background, 1040 px = 520 pt @2x incl. a glow margin) and
/// SplashWord (818x356 px @2x), both from tools/make_splash_assets.py.
struct TVSplashView: View {
    var onFinish: () -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @FocusState private var focused: Bool
    @State private var start = Date.now
    @State private var pills = SplashPill.rain()
    @State private var done = false

    // Timeline (s)
    static let logoStart = 1.35        // logo starts while the last pills are still falling
    static let logoLength = 2.6        // rack focus, half speed
    static let wordStart = 2.75        // wordmark starts as the logo is going sharp
    static let wordLength = 0.3
    static let total = 4.5             // hold, then hand over to the dashboard
    static let reducedTotal = 1.2      // Reduce Motion: no rain, short fades

    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / 1920    // design px -> points
            TimelineView(.animation) { ctx in
                let t = ctx.date.timeIntervalSince(start)
                ZStack {
                    EllipticalGradient(stops: [
                        .init(color: Self.rgb(0x0E2148), location: 0),
                        .init(color: Self.rgb(0x08142C), location: 0.55),
                        .init(color: Self.rgb(0x060E20), location: 1),
                    ], center: UnitPoint(x: 0.5, y: 0.55))

                    if !reduceMotion {
                        Canvas { gc, _ in
                            for p in pills { p.draw(in: gc, t: t, k: k) }
                        }
                    }

                    logo(t: t, k: k)
                        .position(x: 960 * k, y: 1080 * 0.44 * k)
                    word(t: t, k: k)
                        .position(x: 960 * k, y: 787 * k)
                }
            }
        }
        .ignoresSafeArea()
        .focusable()
        .focused($focused)
        .onAppear { focused = true; start = .now }
        .onTapGesture { finish() }               // Select
        .onPlayPauseCommand { finish() }
        .onMoveCommand { _ in finish() }
        .onExitCommand { finish() }              // Menu skips the splash instead of leaving the app
        .task {
            try? await Task.sleep(for: .seconds(reduceMotion ? Self.reducedTotal : Self.total))
            finish()
        }
        .accessibilityElement()
        .accessibilityLabel("Did I?")
    }

    private func finish() {
        guard !done else { return }
        done = true
        onFinish()
    }

    // MARK: Logo: rack focus. Starts huge, soft and blurred, pulls back and snaps sharp.

    @ViewBuilder private func logo(t: Double, k: Double) -> some View {
        let (opacity, scale, blur): (Double, Double, Double) = {
            if reduceMotion {
                return (Self.clamp(t / 0.3), 1, 0)
            }
            let p = Self.rackEase(Self.clamp((t - Self.logoStart) / Self.logoLength))
            return (Self.keys(p, [(0, 0), (0.3, 0.7), (0.85, 1), (1, 1)]),
                    Self.keys(p, [(0, 3.4), (0.3, 2.2), (0.85, 0.97), (1, 1)]),
                    Self.keys(p, [(0, 40), (0.3, 24), (0.85, 0), (1, 0)]))
        }()
        Image("SplashLogo").resizable()
        .frame(width: 520 * k, height: 520 * k)
        .scaleEffect(scale)
        .blur(radius: blur * k)
        .opacity(opacity)
    }

    // MARK: Wordmark: rises 24 px and sharpens from a slight blur.

    @ViewBuilder private func word(t: Double, k: Double) -> some View {
        let p = reduceMotion ? Self.clamp((t - 0.3) / 0.3)
                             : Self.wordEase(Self.clamp((t - Self.wordStart) / Self.wordLength))
        Image("SplashWord").resizable()
            .frame(width: 409 * k, height: 178 * k)     // 818x356 px @2x
            .offset(y: (1 - p) * 24 * k)
            .blur(radius: (1 - p) * 8 * k)
            .opacity(p)
    }

    // MARK: Easing (CSS cubic-bezier, applied to the whole animation like the preview)

    static let rackEase = CubicBezier(0.25, 0.8, 0.25, 1)
    static let wordEase = CubicBezier(0.2, 0.7, 0.2, 1)

    static func clamp(_ x: Double) -> Double { min(max(x, 0), 1) }

    /// Piecewise-linear value at progress p through (offset, value) keyframes.
    static func keys(_ p: Double, _ kf: [(Double, Double)]) -> Double {
        guard p > kf[0].0 else { return kf[0].1 }
        for i in 1..<kf.count where p <= kf[i].0 {
            let (o0, v0) = kf[i - 1], (o1, v1) = kf[i]
            return v0 + (v1 - v0) * (p - o0) / (o1 - o0)
        }
        return kf[kf.count - 1].1
    }

    static func rgb(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255,
              blue: Double(hex & 0xFF) / 255)
    }
}

/// CSS-style cubic-bezier(x1, y1, x2, y2): x is time progress, returns eased progress.
struct CubicBezier {
    let x1, y1, x2, y2: Double
    init(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double) { (self.x1, self.y1, self.x2, self.y2) = (x1, y1, x2, y2) }

    private func bez(_ t: Double, _ a: Double, _ b: Double) -> Double {
        let u = 1 - t
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t
    }

    func callAsFunction(_ x: Double) -> Double {
        if x <= 0 { return 0 }
        if x >= 1 { return 1 }
        var lo = 0.0, hi = 1.0, t = x
        for _ in 0..<30 {                       // bisection: x(t) is monotonic for x1, x2 in 0...1
            let v = bez(t, x1, x2)
            if abs(v - x) < 1e-5 { break }
            if v < x { lo = t } else { hi = t }
            t = (lo + hi) / 2
        }
        return bez(t, y1, y2)
    }
}

// MARK: Falling pills

struct SplashPill {
    enum Kind: CaseIterable { case capsule, tablet, oval, gel, small }
    let kind: Kind
    let c1: Color, c2: Color
    let scale: Double, far: Bool
    let x0: Double, x1: Double, r0: Double, r1: Double
    let delay: Double, duration: Double

    /// Gravity feel: slow start, fast exit.
    static let fall = CubicBezier(0.45, 0, 0.9, 0.55)

    /// Dose palette (firmware/main/dose_colors.c), the brighter half.
    static let palette: [UInt32] = [0x1f45e6, 0x4f46e5, 0xc020e6, 0xdb2777, 0xea580c, 0xd97706, 0x0d9488,
                                    0xdc2626, 0xeab308, 0x16a34a, 0x8ecdf0, 0x65a30d, 0x0891b2, 0xfdba74,
                                    0x86efac, 0xc4b5fd, 0xf472b6]

    /// 26 pills, staggered over ~1.15 s, each falling 1.0-1.5 s (far ones 20% slower). New every launch.
    static func rain(count: Int = 26) -> [SplashPill] {
        let kinds: [Kind] = [.capsule, .capsule, .capsule, .tablet, .oval, .gel, .small, .small]
        return (0..<count).map { i in
            let a = palette.randomElement()!
            var b = palette.randomElement()!
            while b == a { b = palette.randomElement()! }
            let s = Double.random(in: 0.7...1.25), far = s < 0.9
            let x0 = Double.random(in: -60...1860), r0 = Double.random(in: 0...360)
            return SplashPill(kind: kinds.randomElement()!, c1: TVSplashView.rgb(a), c2: TVSplashView.rgb(b),
                              scale: s, far: far,
                              x0: x0, x1: x0 + .random(in: -140...140),
                              r0: r0, r1: r0 + .random(in: -540...540),
                              delay: Double(i) / Double(count) * 0.9 + .random(in: 0...0.25),
                              duration: .random(in: 1.0...1.5) * (far ? 1.2 : 1))
        }
    }

    /// Bounding box of each shape in design px (the preview's SVG sizes).
    var size: CGSize {
        switch kind {
        case .capsule: CGSize(width: 200, height: 78)
        case .tablet: CGSize(width: 120, height: 120)
        case .oval: CGSize(width: 170, height: 96)
        case .gel: CGSize(width: 150, height: 96)
        case .small: CGSize(width: 84, height: 84)
        }
    }

    func draw(in gc: GraphicsContext, t: Double, k: Double) {
        let raw = (t - delay) / duration
        guard raw >= 0, raw <= 1 else { return }
        let p = Self.fall(raw)
        // the preview moves the shape's top-left corner from (x0, -260) to (x1, 1300); rotation and
        // scale are about its center
        let cx = x0 + (x1 - x0) * p + size.width / 2
        let cy = -260 + 1560 * p + size.height / 2
        var g = gc
        if far {
            g.addFilter(.blur(radius: 1.5 * k))
            g.addFilter(.colorMultiply(Color(white: 0.75)))
        } else {
            g.addFilter(.shadow(color: .black.opacity(0.53), radius: 9 * k, x: 0, y: 12 * k))
        }
        g.translateBy(x: cx * k, y: cy * k)
        g.rotate(by: .degrees(r0 + (r1 - r0) * p))
        g.scaleBy(x: scale * k, y: scale * k)
        shape(in: &g)
    }

    private func shape(in g: inout GraphicsContext) {
        let rim = Color(red: 0.78, green: 0.82, blue: 1).opacity(0.45)
        let navy = TVSplashView.rgb(0x08142C)
        let hl = Color.white.opacity(0.28)
        func rr(_ x: Double, _ y: Double, _ w: Double, _ h: Double, _ r: Double) -> Path {
            Path(roundedRect: CGRect(x: x, y: y, width: w, height: h), cornerRadius: r)
        }
        func oval(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> Path {
            Path(ellipseIn: CGRect(x: x, y: y, width: w, height: h))
        }
        switch kind {
        case .capsule:                                   // two-tone, seam, rim, highlight
            let body = rr(-98, -37, 196, 74, 37)
            g.drawLayer { l in
                l.clip(to: body)
                l.fill(Path(CGRect(x: -100, y: -39, width: 100, height: 78)), with: .color(c1))
                l.fill(Path(CGRect(x: 0, y: -39, width: 100, height: 78)), with: .color(c2))
                l.fill(Path(CGRect(x: -2, y: -39, width: 4, height: 78)), with: .color(navy.opacity(0.55)))
            }
            g.stroke(body, with: .color(rim), lineWidth: 3)
            g.fill(rr(-70, -25, 140, 12, 6), with: .color(hl))
        case .tablet:                                    // round, scored, inner ring
            g.fill(oval(-56, -56, 112, 112), with: .color(c1))
            g.stroke(oval(-56, -56, 112, 112), with: .color(rim), lineWidth: 3)
            g.fill(rr(-44, -3, 88, 6, 3), with: .color(navy.opacity(0.35)))
            g.stroke(oval(-44, -44, 88, 88), with: .color(.white.opacity(0.18)), lineWidth: 3)
            g.fill(rr(-28, -36, 50, 10, 5), with: .color(hl))
        case .oval:                                      // oval tablet, scored across
            g.fill(oval(-82, -45, 164, 90), with: .color(c1))
            g.stroke(oval(-82, -45, 164, 90), with: .color(rim), lineWidth: 3)
            g.fill(rr(-5, -36, 10, 72, 5), with: .color(navy.opacity(0.3)))
            g.fill(rr(-55, -28, 100, 10, 5), with: .color(hl))
        case .gel:                                       // translucent softgel
            g.fill(oval(-72, -45, 144, 90), with: .color(c1.opacity(0.72)))
            g.stroke(oval(-72, -45, 144, 90), with: .color(rim), lineWidth: 3)
            g.fill(oval(-56, -32, 112, 64), with: .color(c1.opacity(0.55)))
            g.fill(oval(-49, -27, 52, 18), with: .color(.white.opacity(0.45)))
        case .small:                                     // small round
            g.fill(oval(-39, -39, 78, 78), with: .color(c1))
            g.stroke(oval(-39, -39, 78, 78), with: .color(rim), lineWidth: 3)
            g.fill(rr(-20, -24, 34, 8, 4), with: .color(hl))
        }
    }
}

/// Shows the splash over `content` at launch, then fades it away. The content (the dashboard)
/// loads underneath, so Home data is usually ready when the splash ends.
struct TVSplashHost<Content: View>: View {
    @ViewBuilder var content: Content
    @State private var showSplash = true

    var body: some View {
        ZStack {
            content
            if showSplash {
                TVSplashView { withAnimation(.easeOut(duration: 0.4)) { showSplash = false } }
                    .transition(.opacity)
                    .zIndex(1)
            }
        }
    }
}
#endif
