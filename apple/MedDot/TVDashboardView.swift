#if os(tvOS)
import SwiftUI

/// Apple TV: the bedroom dashboard's layout (dashboard/main/ui.c), read from across the room. A banner
/// with the date and time, then three equal columns: today's doses over the next-dose card | calendar |
/// weather over timer. Dark like the panel, in its colors. Select the card to take the next dose; select
/// a taken row to undo it.
struct TVDashboardView: View {
    @State private var store = HomeStore()
    private let weather = WeatherStore.shared
    @State private var busy = false
    @State private var error: String? = nil
    @State private var confirmUndo: DoseTile? = nil
    /// Overdue alert put off on this TV ("Later"), per dose, until
    @State private var laterUntil: [UUID: Date] = [:]
    /// The remote starts on the next-dose card (not the badge, the first thing in reading order)
    private enum Focus: Hashable { case card }
    @FocusState private var focus: Focus?

    private let ambient = TVAmbient.shared

    /// The 5 s clock for the overlays. Not a TimelineView: a NavigationStack inside one re-laid itself out
    /// nonstop on tvOS (100% CPU on the main thread, the remote nearly frozen).
    @State private var now = Date.now

    var body: some View {
        content(now)
            .task {
                while !Task.isCancelled {
                    try? await Task.sleep(for: .seconds(5))
                    now = .now
                }
            }
    }

    private func content(_ now: Date) -> some View {
        Group {
            let today = store.status == .ready
                ? Today(tiles: store.doses, overdueOpen: store.overdue == true, now: now) : nil
            let due = today?.next.map { $0.state == .due || $0.state == .overdue } ?? false
            ZStack {
                // under the pixel shift: what the moved dashboard uncovers is its own color, not gray
                Dash.bg.ignoresSafeArea()
                // full screen from the first frame: inside the NavigationStack alone, tvOS first laid it
                // out within its safe area (~80 pt sides, 60 top/bottom) and flipped later
                dashboard
                    .ignoresSafeArea()
                    .offset(ambient.shift(at: now))
                if ambient.ambient(at: now, doseDue: due), !TVTimer.shared.done {
                    AmbientScreen(next: today?.next, timer: TVTimer.shared, ambient: ambient)
                        .transition(.opacity)
                }
                if let late = today?.next, late.state == .overdue, (laterUntil[late.id] ?? .distantPast) < now {
                    OverdueAlert(dose: late, busy: busy,
                                 take: { set(late.tile, taken: true) },
                                 later: { laterUntil[late.id] = Date.now.addingTimeInterval(15 * 60) })
                        .transition(.opacity)
                }
                if TVTimer.shared.done {
                    TimerDone(timer: TVTimer.shared)
                }
            }
            .animation(.easeInOut(duration: 0.6), value: ambient.ambient(at: now, doseDue: due))
        }
        .ignoresSafeArea()
    }

    private var dashboard: some View {
        NavigationStack {
            TimelineView(.periodic(from: .distantPast, by: 15)) { context in
                let now = context.date
                VStack(spacing: Dash.pad) {
                    banner(now)
                    switch store.status {
                    case .ready:
                        columns(Today(tiles: store.doses, overdueOpen: store.overdue == true, now: now))
                    case .loading:
                        message("Reading Apple Home…", icon: "house")
                    case .noAccess:
                        message("Did I? needs access to your home. Turn it on in Settings.", icon: "lock")
                    case .noHome:
                        message("No home found", icon: "house")
                    case .noReminder:
                        message("No Pill Reminder in \(store.homeName)", icon: "pills")
                    }
                }
                .padding(.horizontal, Dash.side)
                .padding(.vertical, Dash.top)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Dash.bg)
                .defaultFocus($focus, .card)
            }
            .ignoresSafeArea()
            .preferredColorScheme(.dark)
            .task {
                weather.start()
                CalendarFeed.shared.start()
            }
            .alert("Home didn't take the change", isPresented: Binding(
                get: { error != nil }, set: { if !$0 { error = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(error ?? "")
            }
            .confirmationDialog(confirmUndo.map { "Undo \($0.name) \(Today.time($0.minute))?" } ?? "",
                                isPresented: Binding(get: { confirmUndo != nil },
                                                     set: { if !$0 { confirmUndo = nil } }),
                                titleVisibility: .visible) {
                Button("Undo", role: .destructive) {
                    if let d = confirmUndo { set(d, taken: false) }
                }
                Button("Keep", role: .cancel) {}
            }
        }
    }

    // MARK: Banner

    /// Logo badge | "WED 25 NOV · 2112 hrs" (date blue, time green, as on the panel) | Settings
    private func banner(_ now: Date) -> some View {
        ZStack {
            HStack(spacing: 14) {
                Text(Self.date(now)).foregroundStyle(Dash.date)
                Text("·").foregroundStyle(Dash.muted)
                Text(Self.time(now)).foregroundStyle(Dash.time)
            }
            .font(.system(size: 52, weight: .semibold))
            .monospacedDigit()
            HStack {
                NavigationLink {
                    AboutView()
                } label: {
                    Image("AverakBadge")
                        .resizable()
                        .frame(width: Dash.banner, height: Dash.banner)
                        .clipShape(RoundedRectangle(cornerRadius: Dash.banner * 0.2237, style: .continuous))
                }
                .buttonStyle(FocusRing(radius: Dash.banner * 0.2237, glow: Dash.violet))
                .accessibilityLabel("About")
                Spacer()
                NavigationLink {
                    SettingsView()
                } label: {
                    Label("Settings", systemImage: "gearshape")
                        .font(.system(size: 30, weight: .semibold))
                        .foregroundStyle(Dash.text)
                        .padding(.horizontal, 26)
                        .frame(height: Dash.banner)
                        .background(Dash.tile, in: RoundedRectangle(cornerRadius: 18))
                }
                .buttonStyle(FocusRing(radius: 18, glow: Dash.date))
            }
        }
        .frame(height: Dash.banner)
        // the remote's swipes find the nearest item in an area, not only one straight in line: the
        // calendar and weather can't take focus, so without these a swipe from the doses (right) or
        // the timer (left, up) found nothing
        .focusSection()
    }

    // MARK: Columns

    private func columns(_ today: Today) -> some View {
        HStack(spacing: Dash.pad) {
            VStack(spacing: Dash.pad) {
                DoseList(today: today) { dose in tapRow(dose, today: today) }
                    .frame(maxHeight: .infinity)
                if let next = today.next {
                    NextCard(dose: next, busy: busy) { set(next.tile, taken: true) }
                        .focused($focus, equals: .card)
                        .frame(maxHeight: .infinity)
                } else {
                    DoneCard(tomorrow: store.doses.first?.minute)
                        .frame(maxHeight: .infinity)
                }
            }
            .frame(maxWidth: .infinity)
            .focusSection()

            // select: every event for the next 14 days (the tile shows what fits), feed link at the end
            NavigationLink {
                CalendarAgenda(feed: CalendarFeed.shared)
            } label: {
                Tile(title: "CALENDAR", color: Dash.calendar, titleColor: Dash.calendarTitle) {
                    CalendarTile(feed: CalendarFeed.shared)
                }
            }
            .buttonStyle(FocusRing(glow: Dash.calendarTitle, lift: FocusRing.tile))
            .frame(maxWidth: .infinity)

            VStack(spacing: Dash.pad) {
                // select: Weather location
                NavigationLink {
                    WeatherPlaceView()
                } label: {
                    Tile(title: weather.placeName.map { "WEATHER · \($0.uppercased())" } ?? "WEATHER",
                         color: Dash.weather, titleColor: Dash.weatherTitle) {
                        WeatherTile(weather: weather)
                    }
                }
                .buttonStyle(FocusRing(glow: Dash.weatherTitle, lift: FocusRing.tile))
                Tile(title: "TIMER", color: Dash.timer, titleColor: Dash.timerTitle) {
                    TimerTile(timer: TVTimer.shared)
                }
            }
            .frame(maxWidth: .infinity)
            .focusSection()
        }
    }

    private func message(_ text: String, icon: String) -> some View {
        ContentUnavailableView {
            Label(text, systemImage: icon)
        } actions: {
            NavigationLink {
                SettingsView()
            } label: {
                Label("Settings", systemImage: "gearshape")
            }
        }
        .frame(maxHeight: .infinity)
    }

    // MARK: Actions

    /// A row: taken = ask to undo; the next one (or any due / late) = take; later ones do nothing
    private func tapRow(_ dose: Today.Dose, today: Today) {
        ambient.touch()
        switch dose.state {
        case .taken: confirmUndo = dose.tile
        case .overdue, .due: set(dose.tile, taken: true)
        case .upcoming:
            if dose.id == today.next?.id, dose.minutesFromNow <= 60 { set(dose.tile, taken: true) }
        }
    }

    private func set(_ dose: DoseTile, taken: Bool) {
        ambient.touch()
        busy = true
        Task {
            defer { busy = false }
            do { try await store.setTaken(dose, taken) } catch { self.error = error.localizedDescription }
        }
    }

    private static func date(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "EEE d MMM"
        return f.string(from: d).uppercased()
    }

    private static func time(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HHmm"
        return f.string(from: d) + " hrs"
    }
}

// MARK: - Look (the panel's colors, dashboard/main/ui.c)

private enum Dash {
    static let bg = Color(hex: 0x0b0c0f)
    static let text = Color(hex: 0xe5e7eb)
    static let muted = Color(hex: 0x9ca3af)
    static let tile = Color(hex: 0x1f2937)
    static let date = Color(hex: 0x93c5fd)
    static let time = Color(hex: 0x86efac)
    static let overdue = Color(hex: 0xef4444)
    static let violet = Color(hex: 0x8b5cf6)   // the badge's pill
    static let calendar = Color(hex: 0x134e4a)
    static let calendarTitle = Color(hex: 0x99f6e4)
    static let weather = Color(hex: 0x075985)
    static let weatherTitle = Color(hex: 0xbae6fd)
    static let timer = Color(hex: 0x4c1d95)
    static let timerTitle = Color(hex: 0xc4b5fd)

    static let side: CGFloat = 80     // tvOS title-safe margins
    static let top: CGFloat = 50
    static let pad: CGFloat = 32
    static let banner: CGFloat = 80
    static let radius: CGFloat = 28
    static let rows = 5               // as on the C5 and the panel
}

extension Color {
    init(hex: UInt32) {
        self.init(red: Double(hex >> 16 & 0xff) / 255, green: Double(hex >> 8 & 0xff) / 255,
                  blue: Double(hex & 0xff) / 255)
    }
}

/// tvOS focus: the item the remote is on lifts (grows a little, floats on a soft shadow, as on the
/// Apple TV home screen) and glows in its own color. The item is flattened first so only its outline
/// casts the shadow, not each label inside it.
private struct FocusRing: ButtonStyle {
    var radius: CGFloat = Dash.radius
    var glow: Color = .white
    /// How much the focused item grows; whole tiles use less (`.tile`) so they don't crowd the columns
    var lift: CGFloat = 1.06
    static let tile: CGFloat = 1.02

    func makeBody(configuration: Configuration) -> some View {
        Lift(configuration: configuration, radius: radius, glow: glow, lift: lift)
    }

    private struct Lift: View {
        let configuration: Configuration
        let radius: CGFloat
        let glow: Color
        let lift: CGFloat
        @Environment(\.isFocused) private var focused

        var body: some View {
            // the halo and shadow only on the focused item: a blur behind every button (even hidden)
            // made the remote sluggish on the TV
            configuration.label
                .compositingGroup()
                .background {
                    if focused {
                        // the halo: the item's shape in its color, blurred, a bit larger than the item
                        RoundedRectangle(cornerRadius: radius, style: .continuous)
                            .fill(glow)
                            .padding(-6)
                            .blur(radius: 26)
                            .opacity(0.85)
                            .transition(.opacity)
                    }
                }
                .shadow(color: .black.opacity(focused ? 0.6 : 0), radius: focused ? 24 : 0, y: focused ? 16 : 0)
                .brightness(focused ? 0.04 : 0)
                .scaleEffect(configuration.isPressed ? 1 + (lift - 1) / 3 : focused ? lift : 1)
                .zIndex(focused ? 1 : 0)
                .animation(.easeOut(duration: 0.18), value: focused)
                .animation(.easeOut(duration: 0.1), value: configuration.isPressed)
        }
    }
}

// MARK: - Doses

/// Today's doses, 5 rows like the panel; more than 5: the window starts one row above the next dose
private struct DoseList: View {
    let today: Today
    let tap: (Today.Dose) -> Void

    var body: some View {
        let all = today.doses
        let nextIndex = all.firstIndex { $0.id == today.next?.id } ?? all.count
        let start = all.count <= Dash.rows ? 0 : max(0, min(nextIndex - 1, all.count - Dash.rows))
        let shown = Array(all.dropFirst(start).prefix(Dash.rows))
        GeometryReader { geo in
            let rowH = (geo.size.height - CGFloat(Dash.rows - 1) * 12) / CGFloat(Dash.rows)
            VStack(spacing: 12) {
                if shown.isEmpty {
                    Text("No doses today")
                        .font(.system(size: 30, weight: .semibold))
                        .foregroundStyle(Dash.muted)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(Dash.tile, in: RoundedRectangle(cornerRadius: 16))
                }
                ForEach(shown) { dose in
                    Button { tap(dose) } label: {
                        row(dose, isNext: dose.id == today.next?.id)
                            .frame(height: rowH)
                    }
                    .buttonStyle(FocusRing(radius: 16, glow: DoseColors.color(dose.tile.colorIndex)))
                }
                if !shown.isEmpty { Spacer(minLength: 0) }
            }
        }
    }

    private func row(_ dose: Today.Dose, isNext: Bool) -> some View {
        let color = dose.tile.colorIndex
        return HStack(spacing: 18) {
            Text(Today.time(dose.tile.minute)).monospacedDigit()
            Text(dose.tile.name).lineLimit(1)
            Spacer()
            switch dose.state {
            case .taken: Image(systemName: "checkmark")
            case .overdue: Text("Late")
            case .due: Text("Due")
            case .upcoming: EmptyView()
            }
        }
        .font(.system(size: 30, weight: .semibold))
        .foregroundStyle(DoseColors.text(color))
        .padding(.horizontal, 22)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(DoseColors.color(color), in: RoundedRectangle(cornerRadius: 16))
        .overlay {
            if dose.state == .overdue {
                RoundedRectangle(cornerRadius: 16).strokeBorder(Dash.overdue, lineWidth: 4)
            }
        }
        .opacity(dose.state == .taken ? 0.55 : 1)
    }
}

/// The next dose, big, in its color: time, name, when; select it to take. Overdue: pulsing red border
/// and a white "Late N min" badge, as on the panel.
private struct NextCard: View {
    let dose: Today.Dose
    let busy: Bool
    let take: () -> Void
    @State private var pulse = false

    private var overdue: Bool { dose.state == .overdue }
    private var early: Bool { dose.state == .upcoming && dose.minutesFromNow > 60 }

    var body: some View {
        let fg = DoseColors.text(dose.tile.colorIndex)
        Button(action: take) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .firstTextBaseline) {
                    Text(Today.time(dose.tile.minute))
                        .font(.system(size: 96, weight: .bold))
                        .monospacedDigit()
                        .fixedSize()
                        .layoutPriority(1)
                    Spacer(minLength: 12)
                    Text(Today.when(dose))
                        .font(.system(size: 30, weight: .bold))
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 6)
                        .foregroundStyle(overdue ? Dash.overdue : fg)
                        .background(overdue ? AnyShapeStyle(.white) : AnyShapeStyle(fg.opacity(0.18)),
                                    in: Capsule())
                }
                Text(dose.tile.name)
                    .font(.system(size: 60, weight: .semibold))
                    .lineLimit(2)
                    .minimumScaleFactor(0.6)
                Spacer(minLength: 0)
                Text(busy ? "Taking…" : early ? "Not due yet" : "Select when taken")
                    .font(.system(size: 30, weight: .semibold))
                    .opacity(0.85)
            }
            .foregroundStyle(fg)
            .padding(32)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(DoseColors.color(dose.tile.colorIndex),
                        in: RoundedRectangle(cornerRadius: Dash.radius, style: .continuous))
            .overlay {
                if overdue {
                    RoundedRectangle(cornerRadius: Dash.radius, style: .continuous)
                        .strokeBorder(Dash.overdue, lineWidth: 8)
                        .opacity(pulse ? 1 : 0.35)
                }
            }
        }
        .buttonStyle(FocusRing(glow: overdue ? Dash.overdue : DoseColors.color(dose.tile.colorIndex)))
        .disabled(busy || early)
        .onAppear {
            withAnimation(.easeInOut(duration: 0.8).repeatForever()) { pulse = true }
        }
        .accessibilityLabel("\(dose.tile.name) \(Today.time(dose.tile.minute)), \(Today.when(dose))")
    }
}

private struct DoneCard: View {
    let tomorrow: Int?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("All done today", systemImage: "checkmark.circle.fill")
                .font(.system(size: 48, weight: .semibold))
                .foregroundStyle(Dash.time)
            if let tomorrow {
                Text("Next: tomorrow \(Today.time(tomorrow))")
                    .font(.system(size: 32, weight: .semibold))
                    .foregroundStyle(Dash.muted)
            }
            Spacer(minLength: 0)
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Dash.tile, in: RoundedRectangle(cornerRadius: Dash.radius, style: .continuous))
    }
}

// MARK: - Tiles

/// A panel tile: colored background, title in a lighter shade of it
private struct Tile<Content: View>: View {
    let title: String
    let color: Color
    let titleColor: Color
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(title)
                .font(.system(size: 28, weight: .bold))
                .foregroundStyle(titleColor)
                .lineLimit(1)
                .truncationMode(.tail)
            content
        }
        .padding(28)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(color, in: RoundedRectangle(cornerRadius: Dash.radius, style: .continuous))
    }
}

/// 5-60 min: − / + set it, the time left big, Start / Pause / Resume and Reset (as on the panel)
private struct TimerTile: View {
    let timer: TVTimer

    var body: some View {
        TimelineView(.periodic(from: .distantPast, by: timer.running ? 0.25 : 30)) { context in
            let idle = !timer.running && timer.pausedLeft == nil
            VStack(spacing: 18) {
                HStack(spacing: 20) {
                    button("minus", wide: false) { timer.adjust(-1) }
                        .disabled(!idle || timer.minutes <= TVTimer.step)
                        .opacity(idle ? 1 : 0.3)
                    Text(TVTimer.clock(timer.left(at: context.date)))
                        .font(.system(size: 88, weight: .bold))
                        .monospacedDigit()
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity)
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                    button("plus", wide: false) { timer.adjust(1) }
                        .disabled(!idle || timer.minutes >= TVTimer.maxMinutes)
                        .opacity(idle ? 1 : 0.3)
                }
                HStack(spacing: 20) {
                    button(timer.running ? "Pause" : timer.pausedLeft != nil ? "Resume" : "Start", wide: true) {
                        timer.startOrPause()
                    }
                    button("Reset", wide: true) { timer.reset() }
                        .disabled(idle)
                        .opacity(idle ? 0.4 : 1)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    /// A purple button; "minus" / "plus" are symbols
    private func button(_ title: String, wide: Bool, action: @escaping () -> Void) -> some View {
        Button {
            TVAmbient.shared.touch()
            action()
        } label: {
            Group {
                if wide {
                    Text(title).font(.system(size: 30, weight: .bold))
                } else {
                    Image(systemName: title).font(.system(size: 34, weight: .bold))
                }
            }
            .foregroundStyle(.white)
            .frame(width: wide ? nil : 76, height: 76)
            .frame(maxWidth: wide ? .infinity : nil)
            .background(Color(hex: 0x6d28d9), in: RoundedRectangle(cornerRadius: 18))
        }
        .buttonStyle(FocusRing(radius: 18, glow: Color(hex: 0xa78bfa)))
    }
}

/// A dose turned overdue (Did I? open): over the dashboard and the ambient screen, pulsing red border,
/// the dose in its color, Taken (focused) / Later (15 min, this TV only). The C5's chime now and every
/// 5 min, by day only (2130-0630 silent). Goes away when the dose is taken anywhere.
private struct OverdueAlert: View {
    let dose: Today.Dose
    let busy: Bool
    let take: () -> Void
    let later: () -> Void
    @State private var pulse = false
    @FocusState private var focused: Bool

    var body: some View {
        let color = dose.tile.colorIndex
        ZStack {
            Color.black.opacity(0.92)
            Rectangle()
                .strokeBorder(Dash.overdue, lineWidth: 24)
                .opacity(pulse ? 1 : 0.3)
            VStack(spacing: 44) {
                Label("Dose overdue", systemImage: "exclamationmark.circle.fill")
                    .font(.system(size: 64, weight: .bold))
                    .foregroundStyle(Dash.overdue)
                VStack(spacing: 10) {
                    Text(Today.time(dose.tile.minute))
                        .font(.system(size: 120, weight: .bold))
                        .monospacedDigit()
                    Text(dose.tile.name)
                        .font(.system(size: 64, weight: .semibold))
                        .lineLimit(1)
                    Text(Today.when(dose))
                        .font(.system(size: 34, weight: .bold))
                        .foregroundStyle(Dash.overdue)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                        .background(.white, in: Capsule())
                }
                .foregroundStyle(DoseColors.text(color))
                .padding(.horizontal, 80)
                .padding(.vertical, 40)
                .frame(minWidth: 760)
                .background(DoseColors.color(color), in: RoundedRectangle(cornerRadius: 36, style: .continuous))
                HStack(spacing: 40) {
                    Button(action: take) {
                        Text(busy ? "Taking…" : "Taken")
                            .font(.system(size: 40, weight: .bold))
                            .foregroundStyle(.white)
                            .frame(width: 340, height: 100)
                            .background(Color(hex: 0x16a34a), in: RoundedRectangle(cornerRadius: 24))
                    }
                    .buttonStyle(FocusRing(radius: 24, glow: Color(hex: 0x22c55e)))
                    .focused($focused)
                    .disabled(busy)
                    Button(action: later) {
                        Text("Later")
                            .font(.system(size: 40, weight: .bold))
                            .foregroundStyle(.white)
                            .frame(width: 340, height: 100)
                            .background(Dash.tile, in: RoundedRectangle(cornerRadius: 24))
                    }
                    .buttonStyle(FocusRing(radius: 24, glow: Dash.muted))
                }
                Text("Later hides this on the TV for 15 min")
                    .font(.system(size: 24, weight: .medium))
                    .foregroundStyle(Dash.muted)
            }
        }
        .ignoresSafeArea()
        .focusSection()
        .onAppear {
            focused = true
            TVAmbient.shared.touch()
            withAnimation(.easeInOut(duration: 0.8).repeatForever()) { pulse = true }
        }
        .task(id: dose.id) {
            while !Task.isCancelled {
                if !TVAmbient.night(.now) { Chime.shared.play() }
                try? await Task.sleep(for: .seconds(5 * 60))
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Dose overdue: \(dose.tile.name) \(Today.time(dose.tile.minute)), \(Today.when(dose))")
    }
}

/// Over everything when the timer runs out: pulsing red border, "Timer done", Done (focused)
private struct TimerDone: View {
    let timer: TVTimer
    @State private var pulse = false
    @FocusState private var focused: Bool

    var body: some View {
        ZStack {
            Color.black.opacity(0.9)
            Rectangle()
                .strokeBorder(Dash.overdue, lineWidth: 24)
                .opacity(pulse ? 1 : 0.3)
            VStack(spacing: 50) {
                Image(systemName: "timer")
                    .font(.system(size: 120, weight: .semibold))
                    .foregroundStyle(Dash.overdue)
                Text("Timer done")
                    .font(.system(size: 110, weight: .bold))
                    .foregroundStyle(.white)
                Button {
                    timer.dismiss()
                } label: {
                    Text("Done")
                        .font(.system(size: 40, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 360, height: 100)
                        .background(Dash.overdue, in: RoundedRectangle(cornerRadius: 24))
                }
                .buttonStyle(FocusRing(radius: 24, glow: Dash.overdue))
                .focused($focused)
            }
        }
        .ignoresSafeArea()
        .focusSection()
        .onAppear {
            focused = true
            withAnimation(.easeInOut(duration: 0.7).repeatForever()) { pulse = true }
        }
    }
}

/// The next days as the panel shows them: a band per day (TODAY brighter, TOMORROW, then "MON 30 NOV"),
/// events as chips: filled with the time when timed, outlined when all day or continuing from an
/// earlier day. Today's timed events drop off once over. As many rows as fit.
private struct CalendarTile: View {
    let feed: CalendarFeed

    enum Row: Identifiable {
        case day(String, today: Bool)
        case event(CalendarFeed.Event, time: String?, id: String)
        case none(String)

        var id: String {
            switch self {
            case .day(let t, _): "d" + t
            case .event(_, _, let id): id
            case .none(let t): "n" + t
            }
        }
        var height: CGFloat {
            switch self {
            case .day: 40
            case .event, .none: 50
            }
        }
    }

    var body: some View {
        switch feed.status {
        case .noFeed:
            Placeholder(text: "Add your calendar's feed link in Settings > Calendar feed")
        case .loading:
            Placeholder(text: "Getting the calendar…")
        case .failed(let why):
            Placeholder(text: why)
        case .ready:
            TimelineView(.periodic(from: .distantPast, by: 60)) { context in
                GeometryReader { geo in
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(Self.fit(Self.rows(feed.events, now: context.date), height: geo.size.height)) { row in
                            Self.view(row)
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder
    static func view(_ row: Row) -> some View {
        switch row {
        case .day(let title, let today):
            Text(title)
                .font(.system(size: 22, weight: .bold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .frame(maxWidth: .infinity, minHeight: 40, alignment: .leading)
                .background(today ? Cal.bandToday : Cal.band, in: RoundedRectangle(cornerRadius: 10))
        case .none(let text):
            Text(text)
                .font(.system(size: 24, weight: .medium))
                .foregroundStyle(Dash.calendarTitle.opacity(0.8))
                .padding(.horizontal, 14)
                .frame(height: 50, alignment: .leading)
        case .event(let e, let time, _):
            HStack(spacing: 12) {
                Badge(who: e.who)
                if let time {
                    Text(time).monospacedDigit().foregroundStyle(Dash.calendarTitle)
                }
                Text(e.title).lineLimit(1).foregroundStyle(.white)
                Spacer(minLength: 0)
            }
            .font(.system(size: 26, weight: .semibold))
            .padding(.horizontal, 10)
            .frame(maxWidth: .infinity, minHeight: 50)
            .background {
                let shape = RoundedRectangle(cornerRadius: 12)
                if time != nil { shape.fill(Cal.chip) } else { shape.strokeBorder(Cal.allDayEdge, lineWidth: 2) }
            }
        }
    }

    /// Bands and chips for the coming days; today always (or "Nothing else today"), other days only
    /// with something on them
    static func rows(_ events: [CalendarFeed.Event], now: Date) -> [Row] {
        let cal = Calendar.current
        let today = cal.startOfDay(for: now)
        var out: [Row] = []
        for d in 0..<CalendarFeed.days {
            guard let day = cal.date(byAdding: .day, value: d, to: today),
                  let next = cal.date(byAdding: .day, value: 1, to: day) else { continue }
            var items: [Row] = []
            for e in events where e.start < next && (e.end > day || e.start >= day) {
                let starts = e.start >= day
                if d == 0, !e.allDay, e.end <= now, e.start < now { continue }   // over today
                let time = !e.allDay && starts ? Today.time(cal.component(.hour, from: e.start) * 60
                                                            + cal.component(.minute, from: e.start)) : nil
                items.append(.event(e, time: time, id: "\(d)-\(e.start.timeIntervalSince1970)-\(e.title.hashValue)"))
            }
            // all-day and continuing first, then by time
            items.sort { a, b in
                guard case .event(let x, let tx, _) = a, case .event(let y, let ty, _) = b else { return false }
                if (tx == nil) != (ty == nil) { return tx == nil }
                return x.start < y.start
            }
            if d == 0 {
                out.append(.day("TODAY", today: true))
                out += items.isEmpty ? [.none("Nothing else today")] : items
            } else if !items.isEmpty {
                out.append(.day(Self.dayTitle(d, day), today: false))
                out += items
            }
        }
        return out
    }

    /// Rows that fit; never ends on a band without an event under it
    private static func fit(_ rows: [Row], height: CGFloat) -> [Row] {
        var used: CGFloat = 0
        var out: [Row] = []
        for r in rows {
            let h = r.height + (out.isEmpty ? 0 : 8)
            if used + h > height { break }
            used += h
            out.append(r)
        }
        if case .day = out.last { out.removeLast() }
        return out
    }

    private static func dayTitle(_ offset: Int, _ day: Date) -> String {
        if offset == 1 { return "TOMORROW" }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "EEE d MMM"
        return f.string(from: day).uppercased()
    }
}

/// Calendar selected: every event for the next 14 days in the tile's style (day bands, chips), scrolled
/// with the remote (each event takes focus so the list can move), then "Calendar feed" to change the
/// link. Same rules as the tile: today's timed events drop off when over.
private struct CalendarAgenda: View {
    let feed: CalendarFeed

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text("CALENDAR")
                    .font(.system(size: 34, weight: .bold))
                    .foregroundStyle(Dash.calendarTitle)
                    .padding(.bottom, 10)
                if feed.status == .ready {
                    ForEach(CalendarTile.rows(feed.events, now: .now)) { row in
                        switch row {
                        case .event:
                            Button {} label: { CalendarTile.view(row) }
                                .buttonStyle(FocusRing(radius: 12, glow: Cal.allDayEdge, lift: FocusRing.tile))
                        default:
                            CalendarTile.view(row)
                                .padding(.top, 10)
                        }
                    }
                } else {
                    Text(Self.why(feed.status))
                        .font(.system(size: 28, weight: .medium))
                        .foregroundStyle(Dash.calendarTitle)
                }
                NavigationLink {
                    CalendarFeedView()
                } label: {
                    Label("Calendar feed", systemImage: "link")
                        .font(.system(size: 28, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 26)
                        .frame(height: 70)
                        .background(Cal.band, in: RoundedRectangle(cornerRadius: 18))
                }
                .buttonStyle(FocusRing(radius: 18, glow: Cal.allDayEdge))
                .padding(.top, 30)
            }
            .frame(maxWidth: 1100, alignment: .leading)
            .padding(.vertical, 60)
            .frame(maxWidth: .infinity)
        }
        .background(Dash.calendar.ignoresSafeArea())
    }

    private static func why(_ status: CalendarFeed.Status) -> String {
        switch status {
        case .noFeed: "No calendar yet: add its feed link below."
        case .loading: "Getting the calendar…"
        case .failed(let why): why
        case .ready: ""
        }
    }
}

/// The person an event is for, as a colored initial: "All" (everyone) purple, others a steady color
/// per name (Evan red and Karen royal blue, as on the panel)
private struct Badge: View {
    let who: String?

    var body: some View {
        Text(who.map { String($0.prefix(1)).uppercased() } ?? "")
            .font(.system(size: 20, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: 34, height: 34)
            .background(Self.color(who), in: Circle())
            .opacity(who == nil ? 0 : 1)
    }

    static func color(_ who: String?) -> Color {
        guard let who else { return .clear }
        if ["all", "everyone", "family"].contains(who.lowercased()) { return Color(hex: 0x9333ea) }
        let palette: [UInt32] = [0x16a34a, 0xd97706, 0xdb2777, 0x0891b2, 0xdc2626, 0x1f45e6]
        let sum = who.unicodeScalars.reduce(0) { $0 + Int($1.value) }
        return Color(hex: palette[sum % palette.count])
    }
}

private enum Cal {
    static let band = Color(hex: 0x0f766e)
    static let bandToday = Color(hex: 0x0d9488)
    static let chip = Color(hex: 0x1d6560)
    static let allDayEdge = Color(hex: 0x5eead4)
}

/// "61°" big with the sky next to it, the condition, today's high / low / rain and humidity, then the
/// next 5 hours. Panel-like; the TV has room for the hours.
private struct WeatherTile: View {
    let weather: WeatherStore

    var body: some View {
        switch weather.status {
        case .ready:
            if let w = weather.now { ready(w) }
        case .locating, .loading:
            Placeholder(text: "Getting the weather…")
        case .noLocation:
            Placeholder(text: "Choose a place in Settings > Weather location, or allow location for Did I? in the Apple TV's Settings")
        case .failed:
            Placeholder(text: "Weather isn't available right now")
        }
    }

    private func ready(_ w: WeatherStore.Now) -> some View {
        TimelineView(.periodic(from: .distantPast, by: 60)) { context in
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .center, spacing: 22) {
                    Image(systemName: WeatherStore.symbol(w.code, night: Self.night(context.date)))
                        .symbolRenderingMode(.multicolor)
                        .font(.system(size: 84))
                    Text("\(w.temp)°")
                        .font(.system(size: 96, weight: .bold))
                        .monospacedDigit()
                    Spacer(minLength: 0)
                }
                .foregroundStyle(.white)
                Text(WeatherStore.condition(w.code))
                    .font(.system(size: 34, weight: .semibold))
                    .foregroundStyle(.white)
                VStack(alignment: .leading, spacing: 4) {
                    Text("H \(w.high)° · L \(w.low)°")
                    Text("Rain \(w.rain)% · Humidity \(w.humidity)%")
                }
                .font(.system(size: 30, weight: .semibold))
                .foregroundStyle(Dash.weatherTitle)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                Spacer(minLength: 0)
                HStack(spacing: 0) {
                    ForEach(w.hours) { h in
                        VStack(spacing: 6) {
                            Text(Self.hour(h.time))
                                .font(.system(size: 22, weight: .semibold))
                                .foregroundStyle(Dash.weatherTitle)
                            Image(systemName: WeatherStore.symbol(h.code, night: Self.night(h.time)))
                                .symbolRenderingMode(.multicolor)
                                .font(.system(size: 30))
                                .frame(height: 36)
                            Text("\(h.temp)°")
                                .font(.system(size: 26, weight: .bold))
                                .foregroundStyle(.white)
                        }
                        .frame(maxWidth: .infinity)
                    }
                }
                if context.date.timeIntervalSince(w.fetched) > 3600 {
                    Text("as of \(Self.hour(w.fetched))")
                        .font(.system(size: 20, weight: .medium))
                        .foregroundStyle(Dash.weatherTitle)
                }
            }
        }
    }

    private static func night(_ d: Date) -> Bool {
        let h = Calendar.current.component(.hour, from: d)
        return h < 6 || h >= 19
    }

    /// "2100", 24 h as everywhere else
    private static func hour(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HHmm"
        return f.string(from: d)
    }
}

private struct Placeholder: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 28, weight: .medium))
            .foregroundStyle(.white.opacity(0.55))
            .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
#endif
