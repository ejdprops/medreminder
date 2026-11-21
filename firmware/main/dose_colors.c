/* Dose color palette, shared by the firmware and the dashboard (dashboard/ builds this file too).
 * The index is stored per dose: only append. */
#include "doses.h"

const uint32_t dose_colors[DOSE_COLOR_COUNT] = {
    0x1f45e6, /* royal blue */
    0x4f46e5, /* indigo */
    0xc020e6, /* purple: a deeper take on the logo's magenta-purple */
    0xdb2777, /* pink */
    0xea580c, /* orange */
    0xd97706, /* amber */
    0x0d9488, /* teal */
    0x475569, /* slate */
    0xdc2626, /* red */
    0xeab308, /* yellow */
    0x16a34a, /* green */
    0x8ecdf0, /* pale sky blue */
    0x92400e, /* brown */
    0x9f1239, /* maroon */
    0x65a30d, /* lime */
    0x047857, /* emerald */
    0x0891b2, /* cyan */
    0x1e3a8a, /* navy */
    0xfdba74, /* peach */
    0xfde68a, /* butter */
    0x86efac, /* mint */
    0xc4b5fd, /* lavender */
    0xf9a8d4, /* rose */
    0x1f2937, /* charcoal */
    0xf472b6, /* light pink */
};

/* Picker grid, 6 per row: warm + deep (red, orange, amber, yellow, light pink, maroon), greens to blue
 * (lime, green, emerald, teal, cyan, navy), blues to pink + pastels (royal, indigo, purple, pink,
 * peach, butter). Not offered: slate, pale sky, mint, lavender, rose, charcoal (least vibrant), brown
 * (replaced by light pink). */
const uint8_t dose_color_order[DOSE_PICKER_COUNT] = {
    8, 4, 5, 9, 24, 13,
    14, 10, 15, 6, 16, 17,
    0, 1, 2, 3, 18, 19,
};
