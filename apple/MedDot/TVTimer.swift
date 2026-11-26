#if os(tvOS)
import AVFoundation
import SwiftUI
import UIKit

/// The dashboard panel's timer for the TV: 5-60 min in 5 min steps, Start / Pause / Reset. When it
/// runs out: "Timer done" over everything and the C5's chime (C6-E6-G6) every 10 s for up to 2 min,
/// until dismissed. The screen saver stays off while it runs.
@Observable
final class TVTimer {
    static let shared = TVTimer()

    static let step = 5
    static let maxMinutes = 60

    private(set) var minutes = 10
    private(set) var endsAt: Date? = nil              // running
    private(set) var pausedLeft: TimeInterval? = nil  // paused
    private(set) var done = false

    private var waiter: Task<Void, Never>?
    private var chimes: Task<Void, Never>?
    private var player: AVAudioPlayer?

    var running: Bool { endsAt != nil }

    /// Seconds left: running, paused, or the set time when idle
    func left(at now: Date) -> TimeInterval {
        if let endsAt { return max(0, endsAt.timeIntervalSince(now)) }
        return pausedLeft ?? TimeInterval(minutes * 60)
    }

    func adjust(_ delta: Int) {
        guard endsAt == nil, pausedLeft == nil else { return }
        minutes = min(Self.maxMinutes, max(Self.step, minutes + delta * Self.step))
    }

    func startOrPause() {
        if let endsAt {   // pause
            pausedLeft = max(0, endsAt.timeIntervalSinceNow)
            self.endsAt = nil
            waiter?.cancel()
            idle(true)
            return
        }
        let left = pausedLeft ?? TimeInterval(minutes * 60)
        pausedLeft = nil
        let end = Date.now.addingTimeInterval(left)
        endsAt = end
        idle(false)
        waiter = Task { [weak self] in
            try? await Task.sleep(for: .seconds(left))
            guard !Task.isCancelled else { return }
            self?.finish()
        }
    }

    func reset() {
        waiter?.cancel()
        endsAt = nil
        pausedLeft = nil
        idle(true)
    }

    /// "Timer done" dismissed
    func dismiss() {
        done = false
        chimes?.cancel()
        player?.stop()
        reset()
    }

    private func finish() {
        endsAt = nil
        pausedLeft = nil
        done = true
        chimes = Task { [weak self] in
            for _ in 0..<12 {   // every 10 s for 2 min
                guard let self, !Task.isCancelled, self.done else { return }
                self.chime()
                try? await Task.sleep(for: .seconds(10))
            }
        }
    }

    private func idle(_ allowed: Bool) {
        UIApplication.shared.isIdleTimerDisabled = !allowed
    }

    private func chime() {
        if player == nil {
            try? AVAudioSession.sharedInstance().setCategory(.playback, options: .mixWithOthers)
            player = try? AVAudioPlayer(data: Self.chimeWAV())
            player?.volume = 0.8
        }
        player?.currentTime = 0
        player?.play()
    }

    /// C6-E6-G6 (1047 / 1319 / 1568 Hz), 16 kHz mono, each note decaying: the C5's alert, as a WAV
    private static func chimeWAV() -> Data {
        let rate = 16000
        var samples: [Int16] = []
        for (i, f) in [1047.0, 1319.0, 1568.0].enumerated() {
            let n = i == 2 ? rate * 6 / 10 : rate * 22 / 100
            for k in 0..<n {
                let t = Double(k) / Double(rate)
                let env = exp(-t * (i == 2 ? 4.0 : 7.0)) * min(1, t * 400)
                samples.append(Int16(sin(2 * .pi * f * t) * env * 0.7 * 32767))
            }
        }
        var d = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        let bytes = UInt32(samples.count * 2)
        d.append(contentsOf: "RIFF".utf8); u32(36 + bytes); d.append(contentsOf: "WAVE".utf8)
        d.append(contentsOf: "fmt ".utf8); u32(16); u16(1); u16(1); u32(UInt32(rate)); u32(UInt32(rate * 2)); u16(2); u16(16)
        d.append(contentsOf: "data".utf8); u32(bytes)
        for s in samples { withUnsafeBytes(of: s.littleEndian) { d.append(contentsOf: $0) } }
        return d
    }

    /// "9:41", "60:00"
    static func clock(_ seconds: TimeInterval) -> String {
        let s = Int(seconds.rounded(.up))
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}
#endif
