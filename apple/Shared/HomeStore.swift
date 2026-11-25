import HomeKit
import Observation

/// One dose tile from the Pill Reminder bridge (an On/Off plug per dose, firmware 7e).
struct DoseTile: Identifiable {
    let id: UUID          // HMAccessory.uniqueIdentifier
    let name: String      // NodeLabel "Karen 0930", unless renamed in Home
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

    @ObservationIgnored private let manager = HMHomeManager()
    @ObservationIgnored private var followed: Set<UUID> = []   // characteristics already set up

    static let manufacturer = "Averak Labs"

    override init() {
        super.init()
        manager.delegate = self
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

        var tiles: [DoseTile] = []
        var sensor: Bool? = nil
        for accessory in ours {
            accessory.delegate = self
            for service in accessory.services {
                switch service.serviceType {
                case HMServiceTypeOutlet, HMServiceTypeSwitch, HMServiceTypeLightbulb:
                    guard let power = service.characteristics.first(where: {
                        $0.characteristicType == HMCharacteristicTypePowerState
                    }) else { continue }
                    follow(power)
                    tiles.append(DoseTile(id: accessory.uniqueIdentifier,
                                          name: accessory.name,
                                          room: accessory.room?.name ?? "",
                                          taken: power.value as? Bool))
                case HMServiceTypeContactSensor:
                    guard let contact = service.characteristics.first(where: {
                        $0.characteristicType == HMCharacteristicTypeContactState
                    }) else { continue }
                    follow(contact)
                    // HomeKit ContactSensorState: 0 = contact detected (closed), 1 = open.
                    if let v = contact.value as? Int { sensor = (v == 1) }
                default:
                    continue
                }
            }
        }
        // NodeLabels end in the 24 h time ("Karen 0930", "Evan 0600 TUE"): sort by it.
        doses = tiles.sorted { Self.sortKey($0.name) < Self.sortKey($1.name) }
        overdue = sensor
        status = .ready
    }

    /// Once per characteristic: ask for change notifications and read the current value
    /// (the cached one can be stale). Both answers come back through reload().
    private func follow(_ c: HMCharacteristic) {
        guard followed.insert(c.uniqueIdentifier).inserted else { return }
        if !c.isNotificationEnabled {
            c.enableNotification(true) { _ in }
        }
        c.readValue { _ in
            Task { @MainActor in self.reload() }
        }
    }

    private static func sortKey(_ name: String) -> String {
        let time = name.split(separator: " ").first { $0.count == 4 && $0.allSatisfy(\.isNumber) }
        return (time.map { String($0) } ?? "9999") + name
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
