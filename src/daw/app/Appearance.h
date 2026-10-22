#pragma once

#include <functional>
#include <string>

#include "ui/UI.h"

namespace daw {

// The DAW's look: a preset's colours (Carbon by default: a deep, neutral charcoal with a soft blue
// accent) on the studio shape, shallow and fine-edged so a dense layout stays calm
ui::Theme studioTheme(bool dark = true, const std::string &preset = "Carbon");
// The presets offered: the dark ones first
std::vector<std::string> appearancePresets();

// The user's look, in %APPDATA%\Vibin\theme.toml: the preset it started from, the colours (as
// "#RRGGBB"), dark or light, and every shape and text setting. Reading starts from the studio
// theme, so a missing key keeps its default; values are clamped by the Theme.
std::string themePath();
std::string writeTheme(const ui::Theme &theme, const std::string &preset);
bool readTheme(const std::string &text, ui::Theme &theme, std::string &preset, std::string &error);
bool loadTheme(ui::Theme &theme, std::string &preset, std::string &error); // false (theme unchanged) when missing or unreadable
bool saveTheme(const ui::Theme &theme, const std::string &preset, std::string &error);

// The Appearance window: a floating, non-modal window, so changes show on the app as they are
// made. Presets, dark or light, the accent (swatches and a hue), the background's tint and
// brightness, the surfaces (depth, softness, shadow strength, corners, curvature, edge highlight,
// light angle) and text and layout (contrast, text size, control size, spacing). Changes are saved
// a moment after they stop. `preset` is the app's record of the preset in use, kept up to date.
// Returns the window; onClose runs when it closes.
ui::FloatingWindow &openAppearance(ui::Context &context, std::string &preset, std::function<void()> onClose = {});

}
