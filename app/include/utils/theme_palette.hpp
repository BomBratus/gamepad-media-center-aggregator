/*
    GMCA theme palettes: neutral before sign-in, Stremio purple when connected.
    AppConfig applies the accent tokens to both light and dark Borealis themes.
*/

#pragma once

#include "api/backend.hpp"  // media::BackendType

namespace plenx {

/// 8-bit RGB accent triple. Alpha (e.g. the translucent focus background) is
/// applied in code so one source color drives both the solid and the alpha'd
/// token.
struct AccentRGB {
    unsigned char r, g, b;
};

/// Accent surface of one theme for a single brls::ThemeVariant. Mirrors the
/// VARIANT tokens extracted from AppConfig::initThemes().
struct ThemePalette {
    /// brls/accent, highlight/color1, sidebar/active_item,
    /// button/primary_enabled_background, button/highlight_*_text,
    /// slider/line_filled, color/app, color/focus/bg (at alpha 115).
    AccentRGB accent;
    /// brls/highlight/color2 — brighter top-stop of the focus glow.
    AccentRGB accentGlowTop;
    /// brls/button/primary_enabled_text — label drawn ON the accent fill.
    AccentRGB onAccentText;
    /// brls/list/listItem_value_color — muted/brightened derived accent.
    AccentRGB listValue;
};

/// Dark + light variant of one theme.
struct ThemeColors {
    ThemePalette dark;
    ThemePalette light;
};

/// Neutral pleNx identity used before any server is connected.
const ThemeColors& defaultPalette();

/// Brand palette for a connected backend.
const ThemeColors& backendPalette(media::BackendType type);

}  // namespace plenx
