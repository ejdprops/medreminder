#if os(tvOS)
import MapKit
import SwiftUI

/// Settings > Weather location (this device only): Automatic (the TV's own location) or a place found by
/// city or ZIP code with Apple's place search.
struct WeatherPlaceView: View {
    private let weather = WeatherStore.shared
    @State private var search = PlaceSearch()
    @State private var query = ""
    @State private var resolving = false
    @State private var error: String? = nil
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                Button {
                    weather.choose(nil)
                    dismiss()
                } label: {
                    row(title: "Automatic", subtitle: "This Apple TV's location", checked: weather.chosen == nil)
                }
                if let chosen = weather.chosen {
                    row(title: chosen.name, subtitle: "Chosen place", checked: true)
                }
            }

            Section {
                TextField("City or ZIP code", text: $query)
                    .onChange(of: query) { _, q in search.update(q) }
                ForEach(search.results, id: \.self) { result in
                    Button {
                        choose(result)
                    } label: {
                        row(title: result.title, subtitle: result.subtitle, checked: false)
                    }
                    .disabled(resolving)
                }
            } header: {
                Text("Choose a place")
            } footer: {
                Text("Kept on this Apple TV only. Typing is quicker with the keyboard your iPhone offers.")
            }
        }
        .navigationTitle("Weather location")
        .alert("Place not found", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(error ?? "")
        }
    }

    private func row(title: String, subtitle: String, checked: Bool) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                if !subtitle.isEmpty {
                    Text(subtitle).font(.footnote).foregroundStyle(.secondary)
                }
            }
            Spacer()
            if checked { Image(systemName: "checkmark") }
        }
    }

    /// A suggestion -> its coordinates and a short name ("Springfield, IL") -> the weather's place
    private func choose(_ result: MKLocalSearchCompletion) {
        resolving = true
        Task {
            defer { resolving = false }
            do {
                let response = try await MKLocalSearch(request: MKLocalSearch.Request(completion: result)).start()
                guard let item = response.mapItems.first else { throw URLError(.cannotFindHost) }
                let c = item.location.coordinate
                let name = item.addressRepresentations?.cityWithContext ?? result.title
                weather.choose(.init(name: name, latitude: c.latitude, longitude: c.longitude))
                dismiss()
            } catch {
                self.error = "Try another spelling or a ZIP code."
            }
        }
    }
}

/// Apple's search suggestions for places (cities, ZIP codes), as the text changes
@Observable
private final class PlaceSearch: NSObject, MKLocalSearchCompleterDelegate {
    private(set) var results: [MKLocalSearchCompletion] = []
    private let completer = MKLocalSearchCompleter()

    override init() {
        super.init()
        completer.delegate = self
        completer.resultTypes = .address
    }

    func update(_ text: String) {
        let q = text.trimmingCharacters(in: .whitespaces)
        if q.isEmpty {
            results = []
        } else {
            completer.queryFragment = q
        }
    }

    nonisolated func completerDidUpdateResults(_ completer: MKLocalSearchCompleter) {
        let found = Array(completer.results.prefix(8))
        Task { @MainActor in self.results = found }
    }

    nonisolated func completer(_ completer: MKLocalSearchCompleter, didFailWithError error: Error) {
        Task { @MainActor in self.results = [] }
    }
}
#endif
