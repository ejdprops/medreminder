#if os(iOS)
import SwiftUI

/// iPhone / iPad: the next dose as a big card in its color (tap to take, 10 s undo), then
/// today's doses as colored rows (hold a taken one to undo). Same states as the C5.
struct PhoneDosesView: View {
    @State private var store = HomeStore()
    @State private var undo: (dose: DoseTile, until: Date)? = nil
    @State private var busy = false
    @State private var error: String? = nil

    var body: some View {
        NavigationStack {
            Group {
                switch store.status {
                case .loading:
                    ContentUnavailableView("Reading Apple Home…", systemImage: "house")
                case .noAccess:
                    ContentUnavailableView("Med Dot needs access to your home",
                                           systemImage: "lock",
                                           description: Text("Turn it on in Settings > Privacy & Security > HomeKit."))
                case .noHome:
                    ContentUnavailableView("No home found", systemImage: "house")
                case .noReminder:
                    ContentUnavailableView("No Pill Reminder in \(store.homeName)", systemImage: "pills")
                case .ready:
                    TimelineView(.periodic(from: .now, by: 15)) { context in
                        ready(Today(tiles: store.doses, overdueOpen: store.overdue == true, now: context.date),
                              now: context.date)
                    }
                }
            }
            .navigationTitle("Med Dot")
            .toolbar {
                NavigationLink {
                    SettingsView()
                } label: {
                    Label("Settings", systemImage: "gearshape")
                }
            }
            .alert("Home didn't take the change", isPresented: Binding(
                get: { error != nil }, set: { if !$0 { error = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(error ?? "")
            }
        }
    }

    private func ready(_ today: Today, now: Date) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text(Self.banner(now))
                    .font(.headline)
                    .foregroundStyle(.secondary)

                if let next = today.next {
                    NextCard(dose: next, busy: busy) { set(next.tile, taken: true) }
                } else {
                    DoneCard(tomorrow: store.doses.first?.minute)
                }

                if let undo, undo.until > now {
                    UndoStrip(dose: undo.dose, until: undo.until) { set(undo.dose, taken: false) }
                }

                if !today.doses.isEmpty {
                    Text("Today")
                        .font(.title3.weight(.semibold))
                        .padding(.top, 4)
                    VStack(spacing: 8) {
                        ForEach(today.doses) { dose in
                            DoseRow(dose: dose, isNext: dose.id == today.next?.id)
                                .contextMenu {
                                    if dose.state == .taken {
                                        Button("Undo \(dose.tile.name) \(Today.time(dose.tile.minute))",
                                               systemImage: "arrow.uturn.backward") {
                                            set(dose.tile, taken: false)
                                        }
                                    } else if dose.state != .upcoming || dose.id == today.next?.id {
                                        Button("Mark taken", systemImage: "checkmark") {
                                            set(dose.tile, taken: true)
                                        }
                                    }
                                }
                        }
                    }
                }
            }
            .padding()
        }
    }

    private func set(_ dose: DoseTile, taken: Bool) {
        busy = true
        Task {
            defer { busy = false }
            do {
                try await store.setTaken(dose, taken)
                undo = taken ? (dose, Date.now.addingTimeInterval(10)) : nil
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    /// "WED 25 NOV · 1412 hrs", as on the C5
    private static func banner(_ date: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "EEE d MMM · HHmm 'hrs'"
        return f.string(from: date).uppercased().replacingOccurrences(of: "HRS", with: "hrs")
    }
}

// MARK: - Next dose

private struct NextCard: View {
    let dose: Today.Dose
    let busy: Bool
    let take: () -> Void
    @State private var pulse = false

    private var overdue: Bool { dose.state == .overdue }

    var body: some View {
        let text = DoseColors.text(dose.tile.colorIndex)
        Button(action: take) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(Today.time(dose.tile.minute))
                        .font(.system(size: 56, weight: .bold, design: .rounded))
                        .monospacedDigit()
                    Spacer()
                    Text(Today.when(dose))
                        .font(.headline)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .foregroundStyle(overdue ? DoseColors.overdue : text)
                        .background(overdue ? AnyShapeStyle(.white) : AnyShapeStyle(text.opacity(0.18)),
                                    in: Capsule())
                }
                Text(dose.tile.name)
                    .font(.system(size: 30, weight: .semibold))
                    .lineLimit(2)
                Text(busy ? "Taking…" : "Tap when taken")
                    .font(.subheadline)
                    .opacity(0.85)
                    .padding(.top, 8)
            }
            .foregroundStyle(text)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(20)
            .background(DoseColors.color(dose.tile.colorIndex), in: RoundedRectangle(cornerRadius: 22))
            .overlay {
                if overdue {
                    RoundedRectangle(cornerRadius: 22)
                        .strokeBorder(DoseColors.overdue, lineWidth: 5)
                        .opacity(pulse ? 1 : 0.35)
                }
            }
        }
        .buttonStyle(.plain)
        .disabled(busy || dose.state == .upcoming && dose.minutesFromNow > 60)
        .sensoryFeedback(.success, trigger: busy) { old, new in old && !new }
        .onAppear {
            withAnimation(.easeInOut(duration: 0.8).repeatForever()) { pulse = true }
        }
        .accessibilityLabel("\(dose.tile.name) \(Today.time(dose.tile.minute)), \(Today.when(dose))")
        .accessibilityHint("Marks it taken")
    }
}

private struct DoneCard: View {
    let tomorrow: Int?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("All done today", systemImage: "checkmark.circle.fill")
                .font(.title2.weight(.semibold))
                .foregroundStyle(.green)
            if let tomorrow {
                Text("Next: tomorrow \(Today.time(tomorrow))")
                    .foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(20)
        .background(.fill.tertiary, in: RoundedRectangle(cornerRadius: 22))
    }
}

private struct UndoStrip: View {
    let dose: DoseTile
    let until: Date
    let undo: () -> Void

    var body: some View {
        HStack {
            Text("Took \(dose.name) \(Today.time(dose.minute))")
            Spacer()
            Button("Undo", action: undo)
                .buttonStyle(.borderedProminent)
                .tint(.orange)
        }
        .padding(12)
        .background(.fill.secondary, in: RoundedRectangle(cornerRadius: 14))
    }
}

// MARK: - Today's rows

private struct DoseRow: View {
    let dose: Today.Dose
    let isNext: Bool

    var body: some View {
        DosePill(minute: dose.tile.minute, name: dose.tile.name, color: dose.tile.colorIndex) {
            switch dose.state {
            case .taken: Image(systemName: "checkmark")
            case .overdue: Text("Late").font(.footnote.weight(.bold))
            case .due: Text("Due").font(.footnote.weight(.bold))
            case .upcoming: EmptyView()
            }
        }
        .overlay {
            if dose.state == .overdue {
                RoundedRectangle(cornerRadius: 12).strokeBorder(DoseColors.overdue, lineWidth: 3)
            }
        }
        .opacity(dose.state == .taken ? 0.55 : 1)
    }
}

#Preview {
    PhoneDosesView()
}
#endif
