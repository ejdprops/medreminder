import Foundation

/// Today's doses with the same states the C5 shows. Home doesn't publish the "overdue after"
/// setting, only the overdue sensor: while it is open, every untaken dose whose time has passed
/// counts as overdue (the C5 may call one of the newest "due" for a few more minutes).
struct Today {
    enum State { case taken, overdue, due, upcoming }

    struct Dose: Identifiable {
        let tile: DoseTile
        let state: State
        let minutesFromNow: Int   // negative once its time has passed
        var id: UUID { tile.id }
    }

    let doses: [Dose]     // due today, by time
    let next: Dose?       // the first one not taken

    init(tiles: [DoseTile], overdueOpen: Bool, now: Date, calendar: Calendar = .current) {
        let weekday = calendar.component(.weekday, from: now)
        let nowMinute = calendar.component(.hour, from: now) * 60 + calendar.component(.minute, from: now)
        doses = tiles
            .filter { $0.weekday == nil || $0.weekday == weekday }
            .map { tile in
                let delta = (tile.minute ?? 24 * 60) - nowMinute
                let state: State
                if tile.taken == true { state = .taken }
                else if delta > 0 { state = .upcoming }
                else if overdueOpen { state = .overdue }
                else { state = .due }
                return Dose(tile: tile, state: state, minutesFromNow: delta)
            }
        next = doses.first { $0.state != .taken }
    }

    /// "0930"
    static func time(_ minute: Int?) -> String {
        guard let minute else { return "----" }
        return String(format: "%02d%02d", minute / 60, minute % 60)
    }

    /// "in 25 min", "in 2 h 5 min", "Due now", "Late 42 min"
    static func when(_ dose: Dose) -> String {
        let m = dose.minutesFromNow
        switch dose.state {
        case .taken: return "Taken"
        case .overdue: return "Late \(duration(-m))"
        case .due: return "Due now"
        case .upcoming: return "in \(duration(m))"
        }
    }

    private static func duration(_ minutes: Int) -> String {
        minutes < 60 ? "\(minutes) min"
            : minutes % 60 == 0 ? "\(minutes / 60) h" : "\(minutes / 60) h \(minutes % 60) min"
    }
}
