import SwiftUI

/// Today's doses from Apple Home. Shared by every platform; List adapts to each one.
struct DosesView: View {
    @State private var store = HomeStore()

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("Med Dot")
        }
    }

    @ViewBuilder
    private var content: some View {
        switch store.status {
        case .loading:
            message("Reading Apple Home…", icon: "house")
        case .noAccess:
            #if os(watchOS)
            message("Med Dot needs access to your home. Allow it on your iPhone in Settings > Privacy & Security > HomeKit.", icon: "lock")
            #else
            message("Med Dot needs access to your home. Turn it on in Settings.", icon: "lock")
            #endif
        case .noHome:
            message("No home found.", icon: "house")
        case .noReminder:
            message("No Pill Reminder in \(store.homeName).", icon: "pills")
        case .ready:
            List {
                if store.overdue == true {
                    Label("Dose overdue", systemImage: "exclamationmark.circle.fill")
                        .foregroundStyle(.red)
                }
                ForEach(store.doses) { dose in
                    DoseRow(dose: dose)
                }
                #if os(tvOS)
                Section {
                    NavigationLink {
                        SettingsView()
                    } label: {
                        Label("Settings", systemImage: "gearshape")
                    }
                }
                #endif
            }
        }
    }

    private func message(_ text: String, icon: String) -> some View {
        ContentUnavailableView(text, systemImage: icon)
    }
}

private struct DoseRow: View {
    let dose: DoseTile

    var body: some View {
        HStack {
            Image(systemName: dose.taken == true ? "checkmark.circle.fill" : "circle")
                .foregroundStyle(dose.taken == true ? .green : .secondary)
            VStack(alignment: .leading) {
                Text(dose.name)
                if !dose.room.isEmpty {
                    Text(dose.room)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

#Preview {
    DosesView()
}
