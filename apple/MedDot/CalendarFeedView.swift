#if os(tvOS)
import SwiftUI

/// Settings > Calendar feed (this device only): the household calendar's private .ics link, e.g.
/// Cozi's. Kept in this Apple TV's keychain; only its host is ever shown.
struct CalendarFeedView: View {
    private let feed = CalendarFeed.shared
    @State private var link = ""
    @State private var invalid = false
    @State private var confirmRemove = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                LabeledContent("Feed", value: CalendarFeed.feedHost ?? "Not set")
            }
            Section {
                TextField("Paste the calendar's feed link", text: $link)
                    .onSubmit(save)
                Button("Save link", action: save)
                    .disabled(link.trimmingCharacters(in: .whitespaces).isEmpty)
            } header: {
                Text(feed.hasFeed ? "Replace the link" : "Add a link")
            } footer: {
                Text("Use your calendar's iCalendar (.ics or webcal) link, such as Cozi's calendar feed. Copy it on your iPhone, then paste it into the keyboard your iPhone offers. It stays on this Apple TV only.")
            }
            if feed.hasFeed {
                Section {
                    Button("Remove the calendar", role: .destructive) { confirmRemove = true }
                }
            }
        }
        .navigationTitle("Calendar feed")
        .alert("That isn't a web link", isPresented: $invalid) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("It should start with https:// or webcal://")
        }
        .confirmationDialog("Remove the calendar from this Apple TV?", isPresented: $confirmRemove) {
            Button("Remove", role: .destructive) {
                feed.setFeed(nil)
                dismiss()
            }
        }
    }

    private func save() {
        if feed.setFeed(link) {
            link = ""
            dismiss()
        } else {
            invalid = true
        }
    }
}
#endif
