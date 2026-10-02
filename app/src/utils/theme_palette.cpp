/*
    GMCA — per-backend theme palettes (see theme_palette.hpp).

    Values are { accent, accentGlowTop, onAccentText, listValue }, dark then
    light. Hex are the verified brand colors. The light variant flips the accent
    toward a darker shade so it stays legible on a light background; the dark
    variant is the primary "dark theater" experience.
*/

#include "utils/theme_palette.hpp"

namespace plenx {

// ---- DEFAULT (pleNx neutral) ------------------------------------------------
// Off-white accent #E8E8E8 reads as "white" on the near-black #0D0E11 chrome
// without the glare of pure #FFFFFF as a large fill. Light variant inverts to a
// dark-grey accent so it stays visible on a light background.
static const ThemeColors kDefault = {
    /* dark  */ {{0xE8, 0xE8, 0xE8}, {0xFF, 0xFF, 0xFF}, {0x10, 0x12, 0x16}, {0xC9, 0xC9, 0xC9}},
    /* light */ {{0x3A, 0x3A, 0x3A}, {0x5A, 0x5A, 0x5A}, {0xFF, 0xFF, 0xFF}, {0x77, 0x77, 0x77}},
};

// ---- STREMIO ----------------------------------------------------------------
// Vivid logo-gradient purple #7B5BF5 (the recognizable accent, not the muted
// system token #664181); #A970CD purple-pink tops the glow. White on-accent
// text — the only theme dark enough to need it instead of a dark label.
static const ThemeColors kStremio = {
    /* dark  */ {{0x7B, 0x5B, 0xF5}, {0xA9, 0x70, 0xCD}, {0xFF, 0xFF, 0xFF}, {0xA8, 0x8E, 0xF7}},
    /* light */ {{0x5E, 0x45, 0xC9}, {0x7B, 0x5B, 0xF5}, {0xFF, 0xFF, 0xFF}, {0x6B, 0x53, 0xC9}},
};

const ThemeColors& defaultPalette() { return kDefault; }

const ThemeColors& backendPalette(media::BackendType type) {
    return kStremio;
}

}  // namespace plenx
