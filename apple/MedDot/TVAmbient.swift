#if os(tvOS)
import SwiftUI
import UIKit

/// "Keep Did I? on screen" (Settings > This device, off by default): the Apple TV's screen saver stays
/// off while the app is open, and the screen is protected instead (OLED burn-in):
/// - ambient: after `idleAfter` without a remote press (1 min at night) only a small block drifts over
///   black: the time, the next dose, the timer. Any press, or a dose turning due, brings the dashboard back;
/// - night (2130-0630, the panel's window): the block in dim red;
/// - the full dashboard shifts a few points every 3 min.
@Observable
final class TVAmbient {
    static let shared = TVAmbient()

    static let nightStart = 21 * 60 + 30
    static let nightEnd = 6 * 60 + 30
    private static let key = "tvKeepOnScreen"

    var keepOn: Bool {
        didSet {
            UserDefaults.standard.set(keepOn, forKey: Self.key)
            lastActivity = .now
            applyIdleTimer()
        }
    }
    private(set) var lastActivity = Date.now
    /// The ambient block is up: focus moving onto it is not activity
    var showing = false
    let idleAfter: TimeInterval

    init() {
        keepOn = UserDefaults.standard.bool(forKey: Self.key)
        #if targetEnvironment(simulator)
        // testing: SIMCTL_CHILD_DIDI_AMBIENT_AFTER=20 xcrun simctl launch ...
        idleAfter = ProcessInfo.processInfo.environment["DIDI_AMBIENT_AFTER"].flatMap(TimeInterval.init) ?? 300
        #else
        idleAfter = 300
        #endif
        // every remote move or swipe updates focus; selections call touch() themselves
        NotificationCenter.default.addObserver(forName: UIFocusSystem.didUpdateNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, !self.showing else { return }
                self.touch()
            }
        }
        applyIdleTimer()
    }

    func touch() { lastActivity = .now }

    /// A press on the ambient screen
    func wake() {
        showing = false
        lastActivity = .now
    }

    static func night(_ d: Date) -> Bool {
        let c = Calendar.current.dateComponents([.hour, .minute], from: d)
        let m = (c.hour ?? 0) * 60 + (c.minute ?? 0)
        return m >= nightStart || m < nightEnd
    }

    func ambient(at now: Date, doseDue: Bool) -> Bool {
        guard keepOn, !doseDue else { return false }
        return now.timeIntervalSince(lastActivity) >= (Self.night(now) ? min(60, idleAfter) : idleAfter)
    }

    /// Full dashboard: a few points off, a new spot every 3 min (only with keepOn)
    func shift(at now: Date) -> CGSize {
        guard keepOn else { return .zero }
        let spots: [(CGFloat, CGFloat)] = [(0, 0), (6, 0), (6, 6), (0, 6), (-6, 6), (-6, 0), (-6, -6), (0, -6), (6, -6)]
        let s = spots[Int(now.timeIntervalSince1970 / 180) % spots.count]
        return CGSize(width: s.0, height: s.1)
    }

    /// The screen saver: off with keepOn, or while the timer runs / rings
    func applyIdleTimer() {
        let timer = TVTimer.shared
        UIApplication.shared.isIdleTimerDisabled = keepOn || timer.running || timer.done
    }
}

/// The ambient screen: black, with one block drifting slowly (the time, the next dose, the timer).
/// Any remote button wakes the dashboard.
struct AmbientScreen: View {
    let next: Today.Dose?
    let timer: TVTimer
    let ambient: TVAmbient
    @FocusState private var focused: Bool

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            let now = context.date
            let night = TVAmbient.night(now)
            GeometryReader { geo in
                block(now, night: night)
                    .fixedSize()
                    .position(Self.drift(now, in: geo.size))
                    .animation(.linear(duration: 1), value: now)
            }
        }
        .background(.black)
        .ignoresSafeArea()
        // one invisible focusable thing takes every press
        .overlay {
            Button { ambient.wake() } label: { Color.clear }
                .buttonStyle(Invisible())
                .focused($focused)
                .onMoveCommand { _ in ambient.wake() }
                .onExitCommand { ambient.wake() }
                .onPlayPauseCommand { ambient.wake() }
        }
        .onAppear {
            ambient.showing = true
            focused = true
        }
        .onDisappear { ambient.showing = false }
    }

    private func block(_ now: Date, night: Bool) -> some View {
        let main = night ? Color(hex: 0xb91c1c) : .white.opacity(0.75)
        let dim = night ? Color(hex: 0x7f1d1d) : .white.opacity(0.45)
        return VStack(alignment: .leading, spacing: 14) {
            Text(Self.time(now))
                .font(.system(size: 110, weight: .semibold))
                .monospacedDigit()
                .foregroundStyle(main)
            if let next {
                HStack(spacing: 18) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(night ? Color(hex: 0x7f1d1d) : DoseColors.color(next.tile.colorIndex))
                        .frame(width: 10, height: 44)
                    Text("\(Today.time(next.tile.minute)) \(next.tile.name)")
                        .foregroundStyle(main)
                    Text(Today.when(next))
                        .foregroundStyle(dim)
                }
                .font(.system(size: 40, weight: .semibold))
            } else {
                Text("All done today")
                    .font(.system(size: 40, weight: .semibold))
                    .foregroundStyle(dim)
            }
            if timer.running || timer.pausedLeft != nil {
                HStack(spacing: 14) {
                    Image(systemName: timer.running ? "timer" : "pause.circle")
                    Text(TVTimer.clock(timer.left(at: now)))
                        .monospacedDigit()
                }
                .font(.system(size: 40, weight: .semibold))
                .foregroundStyle(main)
            }
        }
        .padding(30)
    }

    /// A slow wander over the whole screen (two incommensurate sines), keeping the block inside
    private static func drift(_ now: Date, in size: CGSize) -> CGPoint {
        let t = now.timeIntervalSince1970
        let marginX = min(size.width / 2, 420), marginY = min(size.height / 2, 220)
        let x = marginX + (size.width - 2 * marginX) * (sin(t / 97) + 1) / 2
        let y = marginY + (size.height - 2 * marginY) * (sin(t / 61 + 1.3) + 1) / 2
        return CGPoint(x: x, y: y)
    }

    private static func time(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HHmm"
        return f.string(from: d)
    }

    private struct Invisible: ButtonStyle {
        func makeBody(configuration: Configuration) -> some View { configuration.label }
    }
}
#endif
