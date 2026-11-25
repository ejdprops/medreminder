import SwiftUI

@main
struct MedDotApp: App {
    var body: some Scene {
        WindowGroup {
            #if os(iOS)
            PhoneDosesView()
            #else
            DosesView()
            #endif
        }
    }
}
