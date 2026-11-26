import SwiftUI

@main
struct MedDotApp: App {
    /// This device's own appearance (Settings > This device); never shared
    @AppStorage("appearance") private var appearance = Appearance.system.rawValue

    var body: some Scene {
        WindowGroup {
            Group {
                #if os(iOS)
                PhoneDosesView()
                #elseif os(tvOS)
                TVDashboardView()
                #else
                DosesView()
                #endif
            }
            .preferredColorScheme(Appearance(rawValue: appearance)?.scheme)
        }
    }
}
