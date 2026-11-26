#if !os(watchOS)
import CryptoKit
import Foundation
import Network
import Observation
import Security

/// The C5's app link (firmware/main/app_link.c; wire format in app_link_proto.h): the only way the
/// app changes shared state. The schedule and "Overdue after" live on the C5; appearance stays on
/// this device. The C5 is found as _meddot._udp (its SRP service, published by the border routers).
enum LinkProto {
    static let service = "_meddot._udp"
    static let pair: UInt8 = 1, pairWait: UInt8 = 2, pairOK: UInt8 = 3, pairNo: UInt8 = 4
    static let req: UInt8 = 5, resp: UInt8 = 6
    enum Op: UInt8 { case get = 1, set, forget, getDoses, setDose, deleteDose }
    static let daily: UInt8 = 0x7f
    static let overdueChoices = [5, 10, 15, 20, 30, 45, 60, 90, 120]
    /// What the C5's color picker offers, in its order (dose_color_order in dose_colors.c)
    static let pickerColors = [8, 4, 5, 9, 24, 13, 14, 10, 15, 6, 16, 17, 0, 1, 2, 3, 18, 19]
    static let weekdays = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]
}

/// A dose as the C5's editor sees it.
struct LinkDose: Identifiable, Equatable {
    var uid: UInt8 = 0          // 0 = not saved yet
    var minute = 8 * 60
    var days: UInt8 = LinkProto.daily
    var color = LinkProto.pickerColors[12]
    var name = ""
    var id: UInt8 { uid }

    /// "Daily" or "TUE"
    var repeatText: String {
        days == LinkProto.daily ? "Daily" : LinkProto.weekdays[max(0, days.trailingZeroBitCount) % 7]
    }
}

enum LinkError: LocalizedError {
    case notFound, noAnswer, notPaired, refused(String), badValue, badReply

    var errorDescription: String? {
        switch self {
        case .notFound: "The Pill Reminder wasn't found on this network."
        case .noAnswer: "The Pill Reminder didn't answer. Try again in a moment."
        case .notPaired: "This device isn't paired with the Pill Reminder any more. Pair it again."
        case .refused(let why): why
        case .badValue: "The Pill Reminder didn't accept that value."
        case .badReply: "The Pill Reminder sent an answer this app doesn't understand."
        }
    }
}

@Observable
final class AppLink {
    enum State: Equatable {
        case searching
        case notFound
        case unpaired
        case pairing(code: String?)   // nil until the C5 answered
        case paired
    }

    private(set) var state: State = .searching
    private(set) var overdue: Int? = nil
    private(set) var doses: [LinkDose] = []

    @ObservationIgnored private var pairing: Pairing? = Keychain.load()
    @ObservationIgnored private var browser: NWBrowser?
    @ObservationIgnored private var connection: NWConnection?
    @ObservationIgnored private var ready = false
    @ObservationIgnored private var inbox: [[UInt8]] = []
    @ObservationIgnored private var waiter: (id: Int, cont: CheckedContinuation<[UInt8]?, Never>)?
    @ObservationIgnored private var waiterSeq = 0
    @ObservationIgnored private var pairCancelled = false

    // MARK: Finding the C5

    /// Browse for the C5, connect, and load the shared settings and doses if paired.
    func start() async {
        state = .searching
        guard let endpoint = await browse() else {
            state = .notFound
            return
        }
        connect(endpoint)
        if pairing == nil {
            state = .unpaired
            return
        }
        state = .paired
        try? await reload()
    }

    private func browse() async -> NWEndpoint? {
        browser?.cancel()
        let b = NWBrowser(for: .bonjour(type: LinkProto.service, domain: nil), using: .udp)
        browser = b
        return await withCheckedContinuation { (cont: CheckedContinuation<NWEndpoint?, Never>) in
            let once = Once(cont: cont, browser: b)
            b.browseResultsChangedHandler = { results, _ in
                Task { @MainActor in
                    if let first = results.first { once.finish(first.endpoint) }
                }
            }
            b.start(queue: .main)
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(8))
                once.finish(nil)
            }
        }
    }

    /// Ends a browse once: the first result, or nothing when time is up.
    private final class Once {
        private var cont: CheckedContinuation<NWEndpoint?, Never>?
        private let browser: NWBrowser

        init(cont: CheckedContinuation<NWEndpoint?, Never>, browser: NWBrowser) {
            self.cont = cont
            self.browser = browser
        }

        func finish(_ e: NWEndpoint?) {
            guard let c = cont else { return }
            cont = nil
            browser.cancel()
            c.resume(returning: e)
        }
    }

    private func connect(_ endpoint: NWEndpoint) {
        connection?.cancel()
        ready = false
        let c = NWConnection(to: endpoint, using: .udp)
        connection = c
        c.stateUpdateHandler = { [weak self] s in
            Task { @MainActor [weak self] in
                guard let self, self.connection === c else { return }
                if case .ready = s { self.ready = true }
                if case .failed = s { self.ready = false }
            }
        }
        c.start(queue: .main)
        receive(on: c)
    }

    private func receive(on c: NWConnection) {
        c.receiveMessage { [weak self] data, _, _, error in
            Task { @MainActor [weak self] in
                guard let self, self.connection === c else { return }
                if let data { self.deliver([UInt8](data)) }
                if error == nil { self.receive(on: c) }
            }
        }
    }

    private func deliver(_ msg: [UInt8]) {
        guard msg.count >= 4, msg[0] == 0x4d, msg[1] == 0x41, msg[2] == 1 else { return }  // "MA" v1
        if let w = waiter {
            waiter = nil
            w.cont.resume(returning: msg)
        } else if inbox.count < 8 {
            inbox.append(msg)
        }
    }

    /// The next message, or nil after the timeout.
    private func next(timeout: Duration) async -> [UInt8]? {
        if !inbox.isEmpty { return inbox.removeFirst() }
        waiterSeq += 1
        let id = waiterSeq
        return await withCheckedContinuation { cont in
            waiter = (id, cont)
            Task { @MainActor in
                try? await Task.sleep(for: timeout)
                if let w = self.waiter, w.id == id {
                    self.waiter = nil
                    w.cont.resume(returning: nil)
                }
            }
        }
    }

    /// Send, then wait for an answer `accept` likes; resend up to `tries` times.
    private func exchange(_ msg: [UInt8], tries: Int = 4, timeout: Duration = .milliseconds(1200),
                          accept: ([UInt8]) -> Bool) async -> [UInt8]? {
        guard let c = connection else { return nil }
        for _ in 0..<max(tries, 1) {
            if !ready { try? await Task.sleep(for: .milliseconds(300)) }
            inbox.removeAll()
            c.send(content: Data(msg), completion: .idempotent)
            let deadline = ContinuousClock.now + timeout
            while ContinuousClock.now < deadline {
                guard let r = await next(timeout: deadline - ContinuousClock.now) else { break }
                if accept(r) { return r }
            }
        }
        return nil
    }

    // MARK: Pairing

    /// Pair this device: the C5 shows a code; `state` becomes .pairing(code:) with the same code
    /// until someone taps Pair (or Cancel) on the C5, or a minute passes.
    func pair(name: String) async throws {
        pairCancelled = false
        let priv = P256.KeyAgreement.PrivateKey()
        let pub = [UInt8](priv.publicKey.x963Representation)
        let nameBytes = Array(name.unicodeScalars.map { $0.isASCII ? UInt8($0.value) : 0x3f }.prefix(32))
        let msg = [0x4d, 0x41, 1, LinkProto.pair] + pub + [UInt8(nameBytes.count)] + nameBytes
        var key: [UInt8]? = nil
        state = .pairing(code: nil)
        defer { if case .pairing = state { state = pairing == nil ? .unpaired : .paired } }
        let deadline = ContinuousClock.now + .seconds(70)
        while ContinuousClock.now < deadline && !pairCancelled {
            guard let r = await exchange(msg, tries: 1, timeout: .milliseconds(1500), accept: {
                $0.count >= 69 && [LinkProto.pairWait, LinkProto.pairOK, LinkProto.pairNo].contains($0[3])
            }) else { continue }
            if r[3] == LinkProto.pairNo {
                throw LinkError.refused("The Pill Reminder said no: cancelled there, timed out, or another device is pairing.")
            }
            let unitPub = Array(r[4..<69])
            if key == nil {
                let peer = try P256.KeyAgreement.PublicKey(x963Representation: unitPub)
                let shared = try priv.sharedSecretFromKeyAgreement(with: peer)
                let okm = shared.hkdfDerivedSymmetricKey(using: SHA256.self,
                                                         salt: Data("meddot-pair-v1".utf8),
                                                         sharedInfo: Data(pub + unitPub),
                                                         outputByteCount: 36)
                let bytes = okm.withUnsafeBytes { [UInt8]($0) }
                key = Array(bytes[0..<32])
                let code = Self.le32(bytes, 32) % 1_000_000
                state = .pairing(code: String(format: "%03d %03d", code / 1000, code % 1000))
            }
            if r[3] == LinkProto.pairOK, let key {
                guard r.count >= 86, Self.mac(key, Array(r[0..<70])) == Array(r[70..<86]) else {
                    throw LinkError.badReply
                }
                pairing = Pairing(client: r[69], key: Data(key), counter: 0)
                Keychain.save(pairing!)
                state = .paired
                try? await reload()
                return
            }
            try? await Task.sleep(for: .seconds(1))
        }
        if !pairCancelled { throw LinkError.refused("Pairing timed out. Try again and tap Pair on the Pill Reminder.") }
    }

    func cancelPairing() { pairCancelled = true }

    /// Forget the pairing here and on the C5.
    func unpair() async {
        _ = try? await request(.forget)
        pairing = nil
        Keychain.delete()
        overdue = nil
        doses = []
        state = .unpaired
    }

    // MARK: Shared settings and doses

    func reload() async throws {
        let s = try await request(.get)
        if s.count == 4 { overdue = Int(s[2]) }
        doses = try Self.parseDoses(await request(.getDoses))
    }

    func setOverdue(_ minutes: Int) async throws {
        let s = try await request(.set, body: [1, 0x01, UInt8(minutes), 0])
        if s.count == 4 { overdue = Int(s[2]) }
    }

    func save(_ d: LinkDose) async throws {
        var name = Array(d.name.unicodeScalars.map { $0.isASCII ? UInt8($0.value) : 0x3f }.prefix(15))
        name += [UInt8](repeating: 0, count: 16 - name.count)
        let body = [d.uid, UInt8(d.minute & 0xff), UInt8(d.minute >> 8), d.days, UInt8(d.color)] + name
        doses = try Self.parseDoses(await request(.setDose, body: body))
    }

    func delete(_ d: LinkDose) async throws {
        doses = try Self.parseDoses(await request(.deleteDose, body: [d.uid]))
    }

    // MARK: Requests

    private func request(_ op: LinkProto.Op, body: [UInt8] = []) async throws -> [UInt8] {
        guard var p = pairing else { throw LinkError.notPaired }
        guard connection != nil else { throw LinkError.notFound }
        // Must grow on every request, even after this app's data was lost: seconds since 2024 x 16
        let clock = UInt32(max(0, Date().timeIntervalSince1970 - 1_704_067_200) * 16)
        let counter = max(p.counter &+ 1, clock)
        p.counter = counter
        pairing = p
        Keychain.save(p)
        let key = [UInt8](p.key)
        var msg: [UInt8] = [0x4d, 0x41, 1, LinkProto.req, p.client] + Self.le32bytes(counter) + [op.rawValue] + body
        msg += Self.mac(key, msg)
        guard let r = await exchange(msg, accept: {
            $0.count >= 11 && $0[3] == LinkProto.resp && Self.le32($0, 5) == counter
        }) else { throw LinkError.noAnswer }
        let status = r[10]
        if status == 1 {  // unknown client: the C5 forgot this device
            pairing = nil
            Keychain.delete()
            state = .unpaired
            throw LinkError.notPaired
        }
        guard r.count >= 11 + 16, Self.mac(key, Array(r[0..<(r.count - 16)])) == Array(r[(r.count - 16)...]) else {
            throw LinkError.badReply
        }
        let out = Array(r[11..<(r.count - 16)])
        switch status {
        case 0: return out
        case 2: throw LinkError.badValue
        case 4: throw LinkError.refused(String(decoding: out, as: UTF8.self))
        default: throw LinkError.badReply
        }
    }

    private static func parseDoses(_ b: [UInt8]) throws -> [LinkDose] {
        guard let n = b.first, b.count >= 1 + Int(n) * 21 else { throw LinkError.badReply }
        return (0..<Int(n)).map { i in
            let o = 1 + i * 21
            let name = b[(o + 5)..<(o + 21)].prefix { $0 != 0 }
            return LinkDose(uid: b[o], minute: Int(b[o + 1]) | Int(b[o + 2]) << 8, days: b[o + 3],
                            color: Int(b[o + 4]), name: String(decoding: name, as: UTF8.self))
        }
    }

    private static func mac(_ key: [UInt8], _ data: [UInt8]) -> [UInt8] {
        Array(HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key)).prefix(16))
    }

    private static func le32(_ b: [UInt8], _ o: Int) -> UInt32 {
        UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
    }

    private static func le32bytes(_ v: UInt32) -> [UInt8] {
        [UInt8(v & 0xff), UInt8(v >> 8 & 0xff), UInt8(v >> 16 & 0xff), UInt8(v >> 24 & 0xff)]
    }
}

/// The pairing key, in this device's keychain (not synced: each device pairs on its own).
struct Pairing: Codable {
    var client: UInt8
    var key: Data
    var counter: UInt32
}

enum Keychain {
    private static let query: [String: Any] = [
        kSecClass as String: kSecClassGenericPassword,
        kSecAttrService as String: "com.averaklabs.meddot.link",
        kSecAttrAccount as String: "pairing",
    ]

    static func load() -> Pairing? {
        var q = query
        q[kSecReturnData as String] = true
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return try? JSONDecoder().decode(Pairing.self, from: data)
    }

    static func save(_ p: Pairing) {
        guard let data = try? JSONEncoder().encode(p) else { return }
        let attrs: [String: Any] = [kSecValueData as String: data]
        if SecItemUpdate(query as CFDictionary, attrs as CFDictionary) == errSecItemNotFound {
            var q = query
            q[kSecValueData as String] = data
            q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            SecItemAdd(q as CFDictionary, nil)
        }
    }

    static func delete() {
        SecItemDelete(query as CFDictionary)
    }
}
#endif
