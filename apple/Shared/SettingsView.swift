#if !os(watchOS)
import SwiftUI

/// Appearance for this device only (dark mode, like brightness and volume, never leaves a device).
enum Appearance: Int, CaseIterable {
    case system, light, dark

    var title: String {
        switch self {
        case .system: "Automatic"
        case .light: "Light"
        case .dark: "Dark"
        }
    }

    var scheme: ColorScheme? {
        switch self {
        case .system: nil
        case .light: .light
        case .dark: .dark
        }
    }
}

/// Settings: this device's appearance, then what every device shares through the Pill Reminder
/// (the C5): "Overdue after" and the dose schedule.
struct SettingsView: View {
    @AppStorage("appearance") private var appearance = Appearance.system.rawValue
    @State private var link = AppLink()
    @State private var error: String? = nil
    @State private var confirmUnpair = false

    var body: some View {
        Form {
            Section("This device") {
                Picker("Appearance", selection: $appearance) {
                    ForEach(Appearance.allCases, id: \.rawValue) { Text($0.title).tag($0.rawValue) }
                }
            }

            Section {
                reminder
            } header: {
                Text("Pill Reminder")
            } footer: {
                Text("Shared by every device. Changes are saved on the Pill Reminder.")
            }
            Section {
                NavigationLink {
                    AboutView()
                } label: {
                    Label("About", systemImage: "info.circle")
                }
            }
        }
        .navigationTitle("Settings")
        .task { await link.start() }
        .alert("Pill Reminder", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(error ?? "")
        }
    }

    @ViewBuilder
    private var reminder: some View {
        switch link.state {
        case .searching:
            HStack(spacing: 12) {
                ProgressView()
                Text("Looking for the Pill Reminder…")
            }
        case .notFound:
            Text("The Pill Reminder wasn't found. This device needs to be on the home Wi-Fi.")
                .foregroundStyle(.secondary)
            Button("Search again") { Task { await link.start() } }
        case .unpaired:
            Text("Pair this device once to change doses and settings from here.")
                .foregroundStyle(.secondary)
            Button("Pair with the Pill Reminder") { pair() }
        case .pairing(let code):
            VStack(alignment: .leading, spacing: 8) {
                if let code {
                    Text(code)
                        .font(.system(size: 44, weight: .bold, design: .rounded))
                        .monospacedDigit()
                    Text("Tap Pair on the Pill Reminder if it shows this code.")
                } else {
                    HStack(spacing: 12) {
                        ProgressView()
                        Text("Asking the Pill Reminder…")
                    }
                }
            }
            Button("Cancel", role: .cancel) { link.cancelPairing() }
        case .paired:
            Picker("Overdue after", selection: Binding(
                get: { link.overdue ?? 30 },
                set: { v in run { try await link.setOverdue(v) } })) {
                ForEach(LinkProto.overdueChoices, id: \.self) { Text("\($0) min").tag($0) }
            }
            .disabled(link.overdue == nil)
            NavigationLink {
                DoseListView(link: link)
            } label: {
                LabeledContent("Doses", value: "\(link.doses.count)")
            }
            Button("Unpair this device", role: .destructive) { confirmUnpair = true }
                .confirmationDialog("Unpair this device from the Pill Reminder?", isPresented: $confirmUnpair) {
                    Button("Unpair", role: .destructive) { Task { await link.unpair() } }
                }
        }
    }

    private func pair() {
        #if os(tvOS)
        let name = "Apple TV"
        #else
        let name = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        #endif
        run { try await link.pair(name: name) }
    }

    private func run(_ work: @escaping () async throws -> Void) {
        Task {
            do { try await work() } catch { self.error = error.localizedDescription }
        }
    }
}

// MARK: - Doses

struct DoseListView: View {
    let link: AppLink
    @State private var error: String? = nil

    var body: some View {
        ScrollView {
            VStack(spacing: 8) {
                NavigationLink {
                    DoseEditor(link: link, dose: LinkDose())
                } label: {
                    Label("Add a dose", systemImage: "plus")
                        .font(.body.weight(.semibold))
                        .frame(maxWidth: .infinity, minHeight: 48)
                        .background(.fill.tertiary, in: RoundedRectangle(cornerRadius: 12))
                }
                .disabled(link.doses.count >= 16)

                // Like the C5's Doses list: time and name in the dose's color, the weekday for weekly doses
                ForEach(link.doses) { dose in
                    NavigationLink {
                        DoseEditor(link: link, dose: dose)
                    } label: {
                        DosePill(minute: dose.minute, name: dose.name, color: dose.color) {
                            if dose.days != LinkProto.daily {
                                Text(dose.repeatText)
                            }
                            Image(systemName: "chevron.right")
                                .font(.footnote.weight(.bold))
                                .opacity(0.7)
                        }
                    }
                }
            }
            .padding()
        }
        #if os(iOS)
        .buttonStyle(.plain)
        #endif
        .navigationTitle("Doses")
        .refreshable { try? await link.reload() }
    }
}

struct DoseEditor: View {
    let link: AppLink
    @State var dose: LinkDose
    @State private var busy = false
    @State private var error: String? = nil
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section {
                TextField("Name", text: Binding(
                    get: { dose.name },
                    set: { dose.name = String($0.filter(\.isASCII).prefix(15)) }))
                HStack {
                    Picker("Hour", selection: Binding(
                        get: { dose.minute / 60 }, set: { dose.minute = $0 * 60 + dose.minute % 60 })) {
                        ForEach(0..<24, id: \.self) { Text(String(format: "%02d", $0)).tag($0) }
                    }
                    Picker("Minute", selection: Binding(
                        get: { dose.minute % 60 }, set: { dose.minute = dose.minute / 60 * 60 + $0 })) {
                        ForEach(Array(stride(from: 0, to: 60, by: 5)), id: \.self) {
                            Text(String(format: "%02d", $0)).tag($0)
                        }
                    }
                }
                #if os(iOS)
                .pickerStyle(.wheel)
                .frame(height: 140)
                #endif
                Picker("Repeat", selection: $dose.days) {
                    Text("Daily").tag(LinkProto.daily)
                    ForEach(0..<7, id: \.self) { d in
                        Text(Calendar.current.weekdaySymbols[d]).tag(UInt8(1 << d))
                    }
                }
            }

            Section("Color") {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 6), spacing: 14) {
                    ForEach(LinkProto.pickerColors, id: \.self) { c in
                        Button {
                            dose.color = c
                        } label: {
                            Circle()
                                .fill(DoseColors.color(c))
                                .frame(width: 36, height: 36)
                                .overlay {
                                    if dose.color == c {
                                        Image(systemName: "checkmark")
                                            .font(.headline)
                                            .foregroundStyle(DoseColors.text(c))
                                    }
                                }
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Color \(c)")
                    }
                }
                .padding(.vertical, 6)
            }

            Section {
                // How it will look on the home screen
                HStack(spacing: 14) {
                    Text(Today.time(dose.minute)).monospacedDigit()
                    Text(dose.name.isEmpty ? "Name" : dose.name)
                    Spacer()
                }
                .font(.body.weight(.semibold))
                .foregroundStyle(DoseColors.text(dose.color))
                .padding(.horizontal, 14)
                .frame(minHeight: 44)
                .background(DoseColors.color(dose.color), in: RoundedRectangle(cornerRadius: 10))
            }

            if dose.uid != 0 {
                Section {
                    Button("Delete dose", role: .destructive) { confirmDelete = true }
                        .confirmationDialog("Delete \(dose.name) \(Today.time(dose.minute))?",
                                            isPresented: $confirmDelete) {
                            Button("Delete", role: .destructive) { run { try await link.delete(dose) } }
                        }
                }
            }
        }
        .navigationTitle(dose.uid == 0 ? "New dose" : "Edit dose")
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") { run { try await link.save(dose) } }
                    .disabled(busy || dose.name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .disabled(busy)
        .alert("Not saved", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(error ?? "")
        }
    }

    private func run(_ work: @escaping () async throws -> Void) {
        busy = true
        Task {
            defer { busy = false }
            do {
                try await work()
                dismiss()
            } catch {
                self.error = error.localizedDescription
            }
        }
    }
}
#endif
