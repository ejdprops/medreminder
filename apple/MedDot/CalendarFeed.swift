import Foundation
import Security

/// The household's calendar as an iCalendar (.ics) feed, e.g. Cozi's, for the TV's calendar tile: the
/// same rules as the dashboard panel (dashboard/main/calendar.c). The feed URL is private: it lives in
/// this device's keychain only, and titles are never logged. Fetched every 15 min; the next 14 days.
///
/// Supported (what Cozi uses, and a little more): DTSTART/DTEND as a date (all day, DTEND exclusive),
/// a date-time with TZID, in UTC ("Z") or floating (local); RRULE FREQ=DAILY/WEEKLY/MONTHLY/YEARLY
/// with INTERVAL, UNTIL, COUNT, BYDAY ("MO", "2SU", "-1FR"), BYMONTHDAY, BYMONTH; EXDATE.
/// A "Name:" prefix on a title is the person it's for: shown as an initial badge.
@Observable
final class CalendarFeed {
    static let shared = CalendarFeed()
    static let days = 14

    struct Event: Equatable {
        let title: String
        let who: String?      // "Evan" from "Evan: Dentist"
        let start: Date
        let end: Date
        let allDay: Bool
    }

    enum Status: Equatable { case noFeed, loading, ready, failed(String) }

    private(set) var status: Status = .noFeed
    /// Occurrences that touch the next `days` days, by start
    private(set) var events: [Event] = []
    private(set) var fetched: Date? = nil

    private var parsed: [VEvent] = []
    private var task: Task<Void, Never>?
    private var started = false

    var hasFeed: Bool { Self.loadURL() != nil }

    func start() {
        guard !started else { return }
        started = true
        #if targetEnvironment(simulator)
        // The simulator's tvOS keyboard can't paste: a feed link can come in at launch instead
        // (`SIMCTL_CHILD_DIDI_CALENDAR_URL=... xcrun simctl launch ...`), saved like a typed one
        if let url = ProcessInfo.processInfo.environment["DIDI_CALENDAR_URL"], let ok = Self.normalize(url) {
            Self.saveURL(ok.absoluteString)
        }
        #endif
        restart()
    }

    /// Settings: a new feed URL (nil removes it). Returns false if it isn't a web address.
    @discardableResult
    func setFeed(_ text: String?) -> Bool {
        if let text {
            guard let url = Self.normalize(text) else { return false }
            Self.saveURL(url.absoluteString)
        } else {
            Self.saveURL(nil)
        }
        parsed = []
        events = []
        fetched = nil
        if started { restart() }
        return true
    }

    /// Re-expands the parsed feed for today (called as the day goes by; no network)
    func refreshDay() {
        guard !parsed.isEmpty else { return }
        events = Self.expand(parsed, from: Calendar.current.startOfDay(for: .now), days: Self.days)
    }

    private func restart() {
        task?.cancel()
        guard let url = Self.loadURL().flatMap(URL.init(string:)) else {
            #if targetEnvironment(simulator)
            showSample()
            #else
            status = .noFeed
            #endif
            return
        }
        status = .loading
        task = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let ok = await self.fetch(url)
                try? await Task.sleep(for: .seconds(ok ? 15 * 60 : 60))
            }
        }
    }

    private func fetch(_ url: URL) async -> Bool {
        var request = URLRequest(url: url)
        request.setValue("DidI/1.0", forHTTPHeaderField: "User-Agent")   // Cozi answers 403 without one
        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            guard code == 200, let text = String(data: data, encoding: .utf8) else {
                if events.isEmpty { status = .failed(code == 0 ? "Calendar unreachable" : "Calendar answered \(code)") }
                return false
            }
            parsed = Self.parse(text)
            events = Self.expand(parsed, from: Calendar.current.startOfDay(for: .now), days: Self.days)
            fetched = .now
            status = .ready
            return true
        } catch {
            if events.isEmpty { status = .failed("Calendar unreachable") }
            return false
        }
    }

    // MARK: Feed URL (keychain, this device only)

    private static let service = "com.averaklabs.meddot.calendar"

    /// "webcal://..." -> "https://..."; anything else must be http(s)
    static func normalize(_ text: String) -> URL? {
        var t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.lowercased().hasPrefix("webcal://") { t = "https://" + t.dropFirst("webcal://".count) }
        guard let url = URL(string: t), let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http", url.host != nil else { return nil }
        return url
    }

    /// "rest.cozi.com/…" for Settings: the host only, never the private path
    static var feedHost: String? {
        loadURL().flatMap(URL.init(string:))?.host.map { "\($0)/…" }
    }

    private static func loadURL() -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecReturnData as String: true]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private static func saveURL(_ url: String?) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
        SecItemDelete(q as CFDictionary)
        guard let url else { return }
        var add = q
        add[kSecValueData as String] = Data(url.utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }

    // MARK: Parsing

    struct VEvent {
        var title = ""
        var start: Date?
        var end: Date?
        var allDay = false
        var timeZone: TimeZone = .current
        var rule: [String: String] = [:]
        var exdates: [Date] = []
    }

    static func parse(_ text: String) -> [VEvent] {
        // unfold continuation lines (a leading space or tab continues the previous line)
        var lines: [String] = []
        for raw in text.components(separatedBy: .newlines) {
            if let first = raw.first, first == " " || first == "\t", !lines.isEmpty {
                lines[lines.count - 1] += raw.dropFirst()
            } else if !raw.isEmpty {
                lines.append(raw)
            }
        }
        var out: [VEvent] = []
        var cur: VEvent?
        for line in lines {
            if line == "BEGIN:VEVENT" { cur = VEvent(); continue }
            if line == "END:VEVENT" {
                if let e = cur, e.start != nil { out.append(e) }
                cur = nil
                continue
            }
            guard cur != nil, let colon = line.firstIndex(of: ":") else { continue }
            let head = line[..<colon].split(separator: ";").map(String.init)
            let value = String(line[line.index(after: colon)...])
            let name = head.first?.uppercased() ?? ""
            var params: [String: String] = [:]
            for p in head.dropFirst() {
                let kv = p.split(separator: "=", maxSplits: 1).map(String.init)
                if kv.count == 2 { params[kv[0].uppercased()] = kv[1] }
            }
            switch name {
            case "SUMMARY":
                cur?.title = unescape(value)
            case "DTSTART":
                if let (d, allDay, tz) = date(value, params) {
                    cur?.start = d
                    cur?.allDay = allDay
                    cur?.timeZone = tz
                }
            case "DTEND":
                cur?.end = date(value, params)?.0
            case "RRULE":
                for part in value.split(separator: ";") {
                    let kv = part.split(separator: "=", maxSplits: 1).map(String.init)
                    if kv.count == 2 { cur?.rule[kv[0].uppercased()] = kv[1] }
                }
            case "EXDATE":
                for v in value.split(separator: ",") {
                    if let d = date(String(v), params)?.0 { cur?.exdates.append(d) }
                }
            default:
                break
            }
        }
        return out
    }

    private static func unescape(_ s: String) -> String {
        s.replacingOccurrences(of: "\\n", with: " ").replacingOccurrences(of: "\\N", with: " ")
            .replacingOccurrences(of: "\\,", with: ",").replacingOccurrences(of: "\\;", with: ";")
            .replacingOccurrences(of: "\\\\", with: "\\")
    }

    /// "20261125" (all day), "20261125T090000" (TZID or floating), "20261125T150000Z" (UTC)
    private static func date(_ v: String, _ params: [String: String]) -> (Date, Bool, TimeZone)? {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        if v.count == 8 || params["VALUE"]?.uppercased() == "DATE" {
            f.dateFormat = "yyyyMMdd"
            f.timeZone = .current
            return f.date(from: String(v.prefix(8))).map { ($0, true, .current) }
        }
        if v.hasSuffix("Z") {
            f.dateFormat = "yyyyMMdd'T'HHmmss'Z'"
            f.timeZone = TimeZone(identifier: "UTC")
            return f.date(from: v).map { ($0, false, .current) }
        }
        let tz = params["TZID"].flatMap { TimeZone(identifier: $0) } ?? .current
        f.dateFormat = "yyyyMMdd'T'HHmmss"
        f.timeZone = tz
        return f.date(from: v).map { ($0, false, tz) }
    }

    // MARK: Expansion

    static func expand(_ list: [VEvent], from windowStart: Date, days: Int) -> [Event] {
        let local = Calendar.current
        guard let windowEnd = local.date(byAdding: .day, value: days, to: windowStart) else { return [] }
        var out: [Event] = []
        for e in list {
            guard let start = e.start else { continue }
            let length: TimeInterval = e.end.map { $0.timeIntervalSince(start) } ?? (e.allDay ? 86400 : 0)
            let (who, title) = split(e.title)
            for s in occurrences(e, start: start, until: windowEnd) {
                let end = s.addingTimeInterval(max(length, 0))
                // touches the window (an all-day / zero-length event on its start day counts)
                guard end > windowStart || (length == 0 && s >= windowStart), s < windowEnd else { continue }
                out.append(Event(title: title, who: who, start: s, end: end, allDay: e.allDay))
                if out.count >= 256 { break }
            }
        }
        return out.sorted { $0.start < $1.start }
    }

    /// "Evan: Dentist" -> ("Evan", "Dentist"); a prefix is one short word before a colon
    static func split(_ title: String) -> (String?, String) {
        guard let colon = title.firstIndex(of: ":") else { return (nil, title) }
        let who = title[..<colon].trimmingCharacters(in: .whitespaces)
        guard !who.isEmpty, who.count <= 12, !who.contains(" ") else { return (nil, title) }
        return (who, title[title.index(after: colon)...].trimmingCharacters(in: .whitespaces))
    }

    private static let weekdays = ["SU": 1, "MO": 2, "TU": 3, "WE": 4, "TH": 5, "FR": 6, "SA": 7]

    /// Every start of `e` up to `until`, in order (one for a single event)
    private static func occurrences(_ e: VEvent, start: Date, until: Date) -> [Date] {
        guard let freq = e.rule["FREQ"]?.uppercased() else { return [start] }
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = e.timeZone
        let interval = max(1, Int(e.rule["INTERVAL"] ?? "1") ?? 1)
        let count = e.rule["COUNT"].flatMap { Int($0) }
        let ruleUntil = e.rule["UNTIL"].flatMap { u -> Date? in
            date(u, u.count == 8 ? ["VALUE": "DATE"] : [:]).map { parsed in
                u.count == 8 ? cal.date(byAdding: .day, value: 1, to: parsed.0)! : parsed.0   // a date UNTIL includes that day
            }
        }
        let last = min(until, ruleUntil ?? until)
        let byMonth = (e.rule["BYMONTH"] ?? "").split(separator: ",").compactMap { Int($0) }
        let byMonthDay = (e.rule["BYMONTHDAY"] ?? "").split(separator: ",").compactMap { Int($0) }
        // BYDAY: (ordinal or nil, weekday 1-7)
        let byDay: [(Int?, Int)] = (e.rule["BYDAY"] ?? "").split(separator: ",").compactMap { part in
            let p = String(part)
            guard p.count >= 2, let wd = weekdays[String(p.suffix(2)).uppercased()] else { return nil }
            let n = p.dropLast(2)
            return (n.isEmpty ? nil : Int(n), wd)
        }
        let time = cal.dateComponents([.hour, .minute, .second], from: start)
        let excluded = Set(e.exdates.map { e.allDay ? cal.startOfDay(for: $0) : $0 })

        func at(_ day: Date) -> Date? {
            cal.date(bySettingHour: time.hour ?? 0, minute: time.minute ?? 0, second: time.second ?? 0, of: day)
        }
        /// days of a month matching BYMONTHDAY / BYDAY (or the start's day)
        func daysIn(year: Int, month: Int) -> [Date] {
            guard let first = cal.date(from: DateComponents(year: year, month: month, day: 1)),
                  let n = cal.range(of: .day, in: .month, for: first)?.count else { return [] }
            let all = (0..<n).compactMap { cal.date(byAdding: .day, value: $0, to: first) }
            if !byMonthDay.isEmpty {
                return byMonthDay.compactMap { d in
                    let i = d > 0 ? d - 1 : n + d
                    return all.indices.contains(i) ? all[i] : nil
                }.sorted()
            }
            if !byDay.isEmpty {
                var out: [Date] = []
                for (ord, wd) in byDay {
                    let matching = all.filter { cal.component(.weekday, from: $0) == wd }
                    if let ord {
                        let i = ord > 0 ? ord - 1 : matching.count + ord
                        if matching.indices.contains(i) { out.append(matching[i]) }
                    } else {
                        out += matching
                    }
                }
                return out.sorted()
            }
            let d = cal.component(.day, from: start)
            return d <= n ? [all[d - 1]] : []
        }

        var out: [Date] = []
        var emitted = 0
        func emit(_ d: Date) -> Bool {   // false = stop
            if d < start { return true }
            if d > last { return false }
            if let count, emitted >= count { return false }
            emitted += 1
            if !excluded.contains(e.allDay ? cal.startOfDay(for: d) : d) { out.append(d) }
            return true
        }

        let startDay = cal.startOfDay(for: start)
        var period = 0
        loop: while period < 20000 {
            defer { period += interval }
            var candidates: [Date] = []
            switch freq {
            case "DAILY":
                guard let d = cal.date(byAdding: .day, value: period, to: startDay) else { break loop }
                let okMonth = byMonth.isEmpty || byMonth.contains(cal.component(.month, from: d))
                let okDay = byDay.isEmpty || byDay.contains { $0.1 == cal.component(.weekday, from: d) }
                if okMonth && okDay { candidates = [d] }
            case "WEEKLY":
                // the week (Monday first) holding the start, moved on by `period` weeks
                let wd = cal.component(.weekday, from: startDay)
                guard let monday = cal.date(byAdding: .day, value: -((wd + 5) % 7), to: startDay),
                      let week = cal.date(byAdding: .day, value: 7 * period, to: monday) else { break loop }
                let days = byDay.isEmpty ? [wd] : byDay.map(\.1)
                candidates = days.compactMap { d in cal.date(byAdding: .day, value: (d + 5) % 7, to: week) }.sorted()
            case "MONTHLY":
                guard let m = cal.date(byAdding: .month, value: period, to: cal.date(from: cal.dateComponents([.year, .month], from: startDay))!)
                else { break loop }
                let c = cal.dateComponents([.year, .month], from: m)
                if byMonth.isEmpty || byMonth.contains(c.month!) { candidates = daysIn(year: c.year!, month: c.month!) }
            case "YEARLY":
                let y = cal.component(.year, from: startDay) + period
                let months = byMonth.isEmpty ? [cal.component(.month, from: startDay)] : byMonth.sorted()
                candidates = months.flatMap { daysIn(year: y, month: $0) }
            default:
                return [start]
            }
            for day in candidates {
                guard let d = at(day) else { continue }
                if !emit(d) { break loop }
            }
            // stop once the next period starts after the window
            let unit: Calendar.Component = freq == "DAILY" ? .day : freq == "WEEKLY" ? .weekOfYear
                : freq == "MONTHLY" ? .month : .year
            if let next = cal.date(byAdding: unit, value: period + interval, to: startDay),
               cal.date(byAdding: unit, value: -1, to: next).map({ $0 > last }) ?? true { break }
        }
        return out
    }
}

#if targetEnvironment(simulator)
extension CalendarFeed {
    /// The simulator without a feed: made-up events so the tile can be seen (never real ones)
    fileprivate func showSample() {
        let cal = Calendar.current
        let today = cal.startOfDay(for: .now)
        func at(_ day: Int, _ h: Int, _ m: Int = 0) -> Date {
            cal.date(byAdding: .minute, value: day * 1440 + h * 60 + m, to: today)!
        }
        func day(_ d: Int) -> Date { cal.date(byAdding: .day, value: d, to: today)! }
        events = [
            Event(title: "Trash pickup", who: "All", start: day(0), end: day(1), allDay: true),
            Event(title: "Dentist", who: "Alex", start: at(0, 21, 30), end: at(0, 22, 30), allDay: false),
            Event(title: "Soccer practice", who: "Sam", start: at(1, 16), end: at(1, 17, 30), allDay: false),
            Event(title: "Grocery run", who: "Alex", start: at(1, 18), end: at(1, 19), allDay: false),
            Event(title: "Book club", who: "Sam", start: at(2, 19), end: at(2, 21), allDay: false),
            Event(title: "Grandma visiting", who: "All", start: day(3), end: day(5), allDay: true),
            Event(title: "Haircut", who: "Alex", start: at(4, 10), end: at(4, 11), allDay: false),
            Event(title: "Piano lesson", who: "Sam", start: at(6, 15, 30), end: at(6, 16, 30), allDay: false),
        ]
        fetched = .now
        status = .ready
    }
}
#endif
