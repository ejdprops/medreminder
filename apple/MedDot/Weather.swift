import CoreLocation
import Foundation
import MapKit

/// Weather from Open-Meteo (api.open-meteo.com: free, no account or key), as the dashboard panel gets
/// it (dashboard/main/weather.c), for the TV's weather tile. Where: this device's location (asked once)
/// or a place chosen in Settings (kept on this device only, like Appearance). Coordinates are rounded to
/// ~1 km before they leave the device. Refreshed every 15 min (1 min after a failure).
@Observable
final class WeatherStore: NSObject, CLLocationManagerDelegate {
    static let shared = WeatherStore()

    /// A place chosen in Settings; nil = automatic (this device's location)
    struct Place: Codable, Equatable {
        let name: String     // "Springfield, IL"
        let latitude: Double
        let longitude: Double
    }

    struct Hour: Identifiable {
        let time: Date
        let temp: Int
        let code: Int
        let rain: Int
        var id: Date { time }
    }

    struct Day: Identifiable {
        let date: Date       // noon of the day at the place
        let code: Int
        let high: Int
        let low: Int
        let rain: Int        // highest chance that day, %
        var id: Date { date }
    }

    struct Now {
        let temp: Int
        let humidity: Int
        let code: Int
        let high: Int
        let low: Int
        let rain: Int        // today's highest chance, %
        let hours: [Hour]    // the next 24, from the current hour (the tile shows 5)
        let days: [Day]      // today and the next 6
        let fetched: Date
    }

    enum Status: Equatable { case locating, noLocation, loading, ready, failed }

    private(set) var status: Status = .locating
    private(set) var now: Now? = nil
    /// The place the tile shows: the chosen one, or this device's city once looked up
    private(set) var placeName: String? = nil
    private(set) var chosen: Place? = WeatherStore.loadPlace()

    private let location = CLLocationManager()
    private var coordinate: CLLocationCoordinate2D?
    private var task: Task<Void, Never>?

    private var started = false

    func start() {
        guard !started else { return }
        started = true
        location.delegate = self
        location.desiredAccuracy = kCLLocationAccuracyKilometer
        follow()
    }

    /// Settings: a place, or nil for automatic. Refetches at once.
    func choose(_ place: Place?) {
        chosen = place
        if let place, let data = try? JSONEncoder().encode(place) {
            UserDefaults.standard.set(data, forKey: Self.placeKey)
        } else {
            UserDefaults.standard.removeObject(forKey: Self.placeKey)
        }
        task?.cancel()
        task = nil
        coordinate = nil
        now = nil
        placeName = nil
        if started { follow() }
    }

    private func follow() {
        if let chosen {
            placeName = chosen.name
            coordinate = CLLocationCoordinate2D(latitude: chosen.latitude, longitude: chosen.longitude)
            loop()
            return
        }
        status = .locating
        switch location.authorizationStatus {
        case .notDetermined: location.requestWhenInUseAuthorization()
        case .denied, .restricted: status = .noLocation
        default: location.requestLocation()
        }
    }

    private static let placeKey = "weatherPlace"

    private static func loadPlace() -> Place? {
        guard let data = UserDefaults.standard.data(forKey: placeKey) else { return nil }
        return try? JSONDecoder().decode(Place.self, from: data)
    }

    /// This device's city, for the tile's title (Apple's place lookup, once per location)
    private func lookUpCity(_ c: CLLocationCoordinate2D) async {
        guard let request = MKReverseGeocodingRequest(location: CLLocation(latitude: c.latitude, longitude: c.longitude)),
              let item = try? await request.mapItems.first else { return }
        if chosen == nil { placeName = item.addressRepresentations?.cityName }
    }

    // MARK: Location

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let auth = manager.authorizationStatus
        Task { @MainActor in
            guard self.chosen == nil, self.started else { return }
            switch auth {
            case .denied, .restricted: self.status = .noLocation
            case .notDetermined: break
            default: self.location.requestLocation()
            }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let c = locations.last?.coordinate else { return }
        Task { @MainActor in
            guard self.chosen == nil else { return }
            let first = self.coordinate == nil
            self.coordinate = c
            if first {
                self.loop()
                await self.lookUpCity(c)
            }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        Task { @MainActor in
            if self.chosen == nil, self.coordinate == nil { self.status = .noLocation }
        }
    }

    // MARK: Fetch

    private func loop() {
        task = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let ok = await self.fetch()
                try? await Task.sleep(for: .seconds(ok ? 15 * 60 : 60))
            }
        }
    }

    private func fetch() async -> Bool {
        guard let c = coordinate else { return false }
        if now == nil { status = .loading }
        var u = URLComponents(string: "https://api.open-meteo.com/v1/forecast")!
        u.queryItems = [
            .init(name: "latitude", value: String(format: "%.2f", c.latitude)),
            .init(name: "longitude", value: String(format: "%.2f", c.longitude)),
            .init(name: "current", value: "temperature_2m,relative_humidity_2m,weather_code"),
            .init(name: "daily", value: "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"),
            .init(name: "hourly", value: "temperature_2m,weather_code,precipitation_probability"),
            .init(name: "temperature_unit", value: "fahrenheit"),
            .init(name: "timezone", value: "auto"),
            .init(name: "forecast_days", value: "7"),
        ]
        do {
            let (data, response) = try await URLSession.shared.data(from: u.url!)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw URLError(.badServerResponse) }
            now = try Self.parse(data)
            status = .ready
            return true
        } catch {
            if now == nil { status = .failed }
            return false
        }
    }

    private struct Reply: Decodable {
        struct Current: Decodable {
            let temperature_2m: Double
            let relative_humidity_2m: Double
            let weather_code: Int
        }
        struct Daily: Decodable {
            let time: [String]
            let weather_code: [Int]
            let temperature_2m_max: [Double]
            let temperature_2m_min: [Double]
            let precipitation_probability_max: [Double?]
        }
        struct Hourly: Decodable {
            let time: [String]
            let temperature_2m: [Double]
            let weather_code: [Int]
            let precipitation_probability: [Double?]
        }
        let utc_offset_seconds: Int
        let current: Current
        let daily: Daily
        let hourly: Hourly
    }

    private static func parse(_ data: Data) throws -> Now {
        let r = try JSONDecoder().decode(Reply.self, from: data)
        // hourly times are the location's local time, "2026-11-25T21:00"
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd'T'HH:mm"
        f.timeZone = TimeZone(secondsFromGMT: r.utc_offset_seconds)
        let start = Date.now.addingTimeInterval(-59 * 60)   // from the current hour on
        var hours: [Hour] = []
        for (i, t) in r.hourly.time.enumerated() {
            guard let d = f.date(from: t), d > start, hours.count < 24 else { continue }
            hours.append(Hour(time: d, temp: Int(r.hourly.temperature_2m[i].rounded()),
                              code: r.hourly.weather_code[i],
                              rain: Int((r.hourly.precipitation_probability[i] ?? 0).rounded())))
        }
        let fd = DateFormatter()
        fd.locale = Locale(identifier: "en_US_POSIX")
        fd.dateFormat = "yyyy-MM-dd"
        fd.timeZone = f.timeZone
        var days: [Day] = []
        for (i, t) in r.daily.time.enumerated() {
            guard let d = fd.date(from: t), i < r.daily.weather_code.count else { continue }
            // noon, so the day reads the same on this device even if the place is in another time zone
            days.append(Day(date: d.addingTimeInterval(12 * 3600), code: r.daily.weather_code[i],
                            high: Int(r.daily.temperature_2m_max[i].rounded()),
                            low: Int(r.daily.temperature_2m_min[i].rounded()),
                            rain: Int((r.daily.precipitation_probability_max[i] ?? 0).rounded())))
        }
        return Now(temp: Int(r.current.temperature_2m.rounded()),
                   humidity: Int(r.current.relative_humidity_2m.rounded()),
                   code: r.current.weather_code,
                   high: Int((r.daily.temperature_2m_max.first ?? 0).rounded()),
                   low: Int((r.daily.temperature_2m_min.first ?? 0).rounded()),
                   rain: Int(((r.daily.precipitation_probability_max.first ?? nil) ?? 0).rounded()),
                   hours: hours, days: days, fetched: .now)
    }

    // MARK: Codes

    /// WMO weather codes, named as on the panel (dashboard/main/weather.c)
    static func condition(_ code: Int) -> String {
        switch code {
        case 0: "Clear"
        case 1: "Mostly clear"
        case 2: "Partly cloudy"
        case 3: "Cloudy"
        case 45, 48: "Fog"
        case 51, 53, 55: "Drizzle"
        case 56, 57: "Freezing drizzle"
        case 61: "Light rain"
        case 63: "Rain"
        case 65: "Heavy rain"
        case 66, 67: "Freezing rain"
        case 71: "Light snow"
        case 73: "Snow"
        case 75: "Heavy snow"
        case 77: "Snow grains"
        case 80, 81: "Showers"
        case 82: "Heavy showers"
        case 85, 86: "Snow showers"
        case 95: "Thunderstorm"
        case 96, 99: "Storm, hail"
        default: ""
        }
    }

    /// SF Symbol for a WMO code (multicolor), at night the moon variants
    static func symbol(_ code: Int, night: Bool = false) -> String {
        switch code {
        case 0, 1: night ? "moon.stars.fill" : "sun.max.fill"
        case 2: night ? "cloud.moon.fill" : "cloud.sun.fill"
        case 3: "cloud.fill"
        case 45, 48: "cloud.fog.fill"
        case 51...57: "cloud.drizzle.fill"
        case 61...67, 80...82: "cloud.rain.fill"
        case 71...77, 85, 86: "cloud.snow.fill"
        case 95...99: "cloud.bolt.rain.fill"
        default: "cloud.fill"
        }
    }
}
