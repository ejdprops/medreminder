#if !os(watchOS)
import SwiftUI

/// The Averak Labs brand mark (the logo over the pill, as the app icon), with app-icon corners.
struct AverakLogo: View {
    var height: CGFloat

    var body: some View {
        Image("AverakLogo")
            .resizable()
            .frame(width: height, height: height)
            .clipShape(RoundedRectangle(cornerRadius: height * 0.2237, style: .continuous))
            .accessibilityLabel("Averak Labs")
    }
}

/// About: the app, the Pill Reminder it reads (from Apple Home) and who made them.
struct AboutView: View {
    @State private var home = HomeStore()

    private var appVersion: String {
        let info = Bundle.main.infoDictionary
        let v = info?["CFBundleShortVersionString"] as? String ?? "?"
        let b = info?["CFBundleVersion"] as? String ?? "?"
        return "\(v) (\(b))"
    }

    var body: some View {
        Form {
            Section {
                HStack {
                    Spacer()
                    AverakLogo(height: 220)
                    Spacer()
                }
                .listRowBackground(Color.clear)
            }

            Section("App") {
                LabeledContent("Name", value: "Did I?")
                LabeledContent("Version", value: appVersion)
                LabeledContent("Made by", value: "Averak Labs")
            }

            Section("Pill Reminder") {
                if let r = home.reminder {
                    LabeledContent("Name in Home", value: r.name)
                    if !r.model.isEmpty { LabeledContent("Model", value: r.model) }
                    if !r.firmware.isEmpty { LabeledContent("Firmware", value: r.firmware) }
                    LabeledContent("Home", value: home.homeName)
                    LabeledContent("Doses in Home", value: "\(home.doses.count)")
                } else {
                    Text(home.status == .loading ? "Reading Apple Home…" : "No Pill Reminder in Apple Home")
                        .foregroundStyle(.secondary)
                }
            }

            Section {
                Text("Did I? shows the doses your Pill Reminder publishes to Apple Home, and changes its schedule over your home network once paired.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("About")
    }
}
#endif
