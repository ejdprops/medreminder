import SwiftUI

/// The dose palette, copied from firmware/main/dose_colors.c (append-only there; keep this in step).
/// The C5 puts each dose's index in its SerialNumber ("K-0930-TUE-C03").
enum DoseColors {
    static let palette: [UInt32] = [
        0x1f45e6, 0x4f46e5, 0xc020e6, 0xdb2777, 0xea580c, 0xd97706, 0x0d9488, 0x475569,
        0xdc2626, 0xeab308, 0x16a34a, 0x8ecdf0, 0x92400e, 0x9f1239, 0x65a30d, 0x047857,
        0x0891b2, 0x1e3a8a, 0xfdba74, 0xfde68a, 0x86efac, 0xc4b5fd, 0xf9a8d4, 0x1f2937,
        0xf472b6,
    ]

    /// Unknown index (older C5 firmware, or not read yet): slate.
    static func rgb(_ index: Int?) -> UInt32 {
        guard let index, palette.indices.contains(index) else { return 0x475569 }
        return palette[index]
    }

    static func color(_ index: Int?) -> Color {
        let c = rgb(index)
        return Color(red: Double((c >> 16) & 0xff) / 255,
                     green: Double((c >> 8) & 0xff) / 255,
                     blue: Double(c & 0xff) / 255)
    }

    /// Same rule as ui_dose_row.c: dark text on light colors (yellow, pale sky, pastels).
    static func text(_ index: Int?) -> Color {
        let c = rgb(index)
        let luma = (299 * Int((c >> 16) & 0xff) + 587 * Int((c >> 8) & 0xff) + 114 * Int(c & 0xff)) / 1000
        return luma > 150 ? Color(red: 0x11 / 255, green: 0x18 / 255, blue: 0x27 / 255) : .white
    }

    static let overdue = Color(red: 0xef / 255, green: 0x44 / 255, blue: 0x44 / 255)
}
