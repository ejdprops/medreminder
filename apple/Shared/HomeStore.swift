import HomeKit
import Observation

/// One dose tile from the Pill Reminder bridge (an On/Off plug per dose, firmware 7e).
struct DoseTile: Identifiable {
    let id: UUID          // HMAccessory.uniqueIdentifier
    let label: String     // accessory name: NodeLabel "Karen 0930", unless renamed in Home
    let name: String      // label without the time / weekday: "Karen"
    let minute: Int?      // minute of the day the dose is due
    let weekday: Int?     // Calendar weekday (1 = Sunday) for weekly doses, nil = daily
    let colorIndex: Int?  // dose palette index, from the SerialNumber
    let room: String
    let taken: Bool?      // nil until HomeKit has a value
}

/// Reads the Pill Reminder's accessories from Apple Home and follows their changes.
@Observable
final class HomeStore: NSObject {
    enum Status: Equatable {
        case loading
        case noAccess          // the user said no, or HomeKit is restricted
        case noHome
        case noReminder        // homes exist, but no Averak Labs accessories in them
        case ready
    }

    private(set) var status: Status = .loading
    private(set) var homeName = ""
    private(set) var doses: [DoseTile] = []
    private(set) var overdue: Bool? = nil   // the "Dose overdue" contact sensor: open = overdue
    /// The Pill Reminder itself (the Matter bridge): name in Home, model, firmware version
    private(set) var reminder: (name: String, model: String, firmware: String)? = nil

    @ObservationIgnored private let manager = HMHomeManager()
    @ObservationIgnored private var followed: Set<UUID> = []   // characteristics already set up
    @ObservationIgnored private var power: [UUID: HMCharacteristic] = [:]  // dose id -> On/Off

    static let manufacturer = "Averak Labs"
    /// HAP Serial Number characteristic. HMCharacteristicTypeSerialNumber is deprecated (no
    /// HMAccessory property replaced it) but the characteristic is still there and readable.
    private static let serialNumberType = "00000030-0000-1000-8000-0026BB765291"

    override init() {
        super.init()
        manager.delegate = self
    }

    /// Take (true) or undo (false) a dose, like its switch in Home. The C5 refuses doses not due
    /// today and sets the switch back; the change comes back through the notifications.
    func setTaken(_ dose: DoseTile, _ taken: Bool) async throws {
        guard let c = power[dose.id] else { return }
        try await c.writeValue(taken)
    }

    private func reload() {
        if manager.authorizationStatus.contains(.determined),
           !manager.authorizationStatus.contains(.authorized) {
            status = .noAccess
            return
        }
        guard let home = manager.homes.first(where: { $0.isPrimary }) ?? manager.homes.first else {
            status = .noHome
            return
        }
        homeName = home.name

        let ours = home.accessories.filter { $0.manufacturer == Self.manufacturer }
        guard !ours.isEmpty else {
            status = .noReminder
            doses = []
            overdue = nil
            return
        }

        if let bridge = ours.first(where: { !$0.isBridged }) {
            reminder = (bridge.name, bridge.model ?? "", bridge.firmwareVersion ?? "")
        }

        var tiles: [DoseTile] = []
        var sensor: Bool? = nil
        power = [:]
        for accessory in ours {
            accessory.delegate = self
            let serial = accessory.services
                .first { $0.serviceType == HMServiceTypeAccessoryInformation }?
                .characteristics.first { $0.characteristicType == Self.serialNumberType }
            if let serial { follow(serial, notify: false) }
            for service in accessory.services {
                switch service.serviceType {
                case HMServiceTypeOutlet, HMServiceTypeSwitch, HMServiceTypeLightbulb:
                    guard let on = service.characteristics.first(where: {
                        $0.characteristicType == HMCharacteristicTypePowerState
                    }) else { continue }
                    follow(on, notify: true)
                    power[accessory.uniqueIdentifier] = on
                    tiles.append(Self.tile(accessory, serial: serial?.value as? String,
                                           taken: on.value as? Bool))
                case HMServiceTypeContactSensor:
                    guard let contact = service.characteristics.first(where: {
                        $0.characteristicType == HMCharacteristicTypeContactState
                    }) else { continue }
                    follow(contact, notify: true)
                    // HomeKit ContactSensorState: 0 = contact detected (closed), 1 = open.
                    if let v = contact.value as? Int { sensor = (v == 1) }
                default:
                    continue
                }
            }
        }
        doses = tiles.sorted { ($0.minute ?? 9999, $0.label) < ($1.minute ?? 9999, $1.label) }
        overdue = sensor
        status = .ready
    }

    /// Once per characteristic: read the current value (the cached one can be stale) and, for
    /// values that change, ask for notifications. Both answers come back through reload().
    private func follow(_ c: HMCharacteristic, notify: Bool) {
        guard followed.insert(c.uniqueIdentifier).inserted else { return }
        if notify && !c.isNotificationEnabled {
            c.enableNotification(true) { _ in }
        }
        c.readValue { _ in
            Task { @MainActor in self.reload() }
        }
    }

    // MARK: Parsing

    private static let weekdays = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]

    /// Time, weekday and color come from the SerialNumber the C5 sets ("K-0930-TUE-C03"), which
    /// a rename in Home doesn't touch; the label ("Karen 0930 TUE") is the fallback for the time.
    private static func tile(_ a: HMAccessory, serial: String?, taken: Bool?) -> DoseTile {
        var minute: Int? = nil
        var weekday: Int? = nil
        var color: Int? = nil
        if let serial {
            for part in serial.split(separator: "-") {
                if let m = minuteOfDay(part) { minute = m }
                if let w = weekdays.firstIndex(of: String(part)) { weekday = w + 1 }
                if part.hasPrefix("C"), let c = Int(part.dropFirst()) { color = c }
            }
        }
        var words = a.name.split(separator: " ")
        while let last = words.last, minuteOfDay(last) != nil || weekdays.contains(String(last)) {
            if minute == nil, let m = minuteOfDay(last) { minute = m }
            if weekday == nil, let w = weekdays.firstIndex(of: String(last)) { weekday = w + 1 }
            words.removeLast()
        }
        return DoseTile(id: a.uniqueIdentifier,
                        label: a.name,
                        name: words.isEmpty ? a.name : words.joined(separator: " "),
                        minute: minute,
                        weekday: weekday,
                        colorIndex: color,
                        room: a.room?.name ?? "",
                        taken: taken)
    }

    /// "0930" -> 570
    private static func minuteOfDay(_ s: Substring) -> Int? {
        guard s.count == 4, s.allSatisfy(\.isASCII), let v = Int(s) else { return nil }
        let h = v / 100, m = v % 100
        return h < 24 && m < 60 ? h * 60 + m : nil
    }
}

extension HomeStore: HMHomeManagerDelegate {
    nonisolated func homeManagerDidUpdateHomes(_ manager: HMHomeManager) {
        Task { @MainActor in self.reload() }
    }

    nonisolated func homeManager(_ manager: HMHomeManager,
                                 didUpdate status: HMHomeManagerAuthorizationStatus) {
        Task { @MainActor in self.reload() }
    }
}

extension HomeStore: HMAccessoryDelegate {
    nonisolated func accessory(_ accessory: HMAccessory, service: HMService,
                               didUpdateValueFor characteristic: HMCharacteristic) {
        Task { @MainActor in self.reload() }
    }

    nonisolated func accessoryDidUpdateName(_ accessory: HMAccessory) {
        Task { @MainActor in self.reload() }
    }
}
