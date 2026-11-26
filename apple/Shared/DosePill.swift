import SwiftUI

/// A dose as a solid row in its color, the way the C5 and the home page show it: time, name, and
/// whatever goes on the right (a check, "Late", a weekday, a chevron).
struct DosePill<Trailing: View>: View {
    let minute: Int?
    let name: String
    let color: Int?
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 14) {
            Text(Today.time(minute))
                .monospacedDigit()
            Text(name)
                .lineLimit(1)
            Spacer()
            trailing
        }
        .font(.body.weight(.semibold))
        .foregroundStyle(DoseColors.text(color))
        .padding(.horizontal, 16)
        .frame(minHeight: 48)
        .background(DoseColors.color(color), in: RoundedRectangle(cornerRadius: 12))
        .contentShape(RoundedRectangle(cornerRadius: 12))
        .accessibilityElement(children: .combine)
    }
}
