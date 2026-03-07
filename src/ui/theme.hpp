#pragma once
#include <ftxui/screen/color.hpp>
#include <ftxui/dom/elements.hpp>

// ╭──────────────────────────────────────────────────╮
// │   Buildm-on · Aurora Pastel Theme                │
// │   All colors are 24-bit RGB true color           │
// ╰──────────────────────────────────────────────────╯

namespace Theme {

using namespace ftxui;

// Helper to avoid initializer list issues with certain compilers/FTXUI versions
template<typename... Args>
inline Elements make_elements(Args... args) {
    Elements el;
    (el.push_back(std::move(args)), ...);
    return el;
}

// ── Backgrounds ──────────────────────────────────────
inline const Color BG        = Color::RGB(15,  15,  20);   // #0f0f14  deepest bg
inline const Color BG2       = Color::RGB(21,  21,  30);   // #15151e  panel bg
inline const Color Surface   = Color::RGB(30,  30,  46);   // #1e1e2e  card surface
inline const Color Overlay   = Color::RGB(38,  38,  58);   // #26263a  header/overlay
inline const Color FocusedBG = Color::RGB(45,  45,  70);   // #2d2d46  input highlight
inline const Color Border    = Color::RGB(42,  42,  62);   // #2a2a3e  subtle border
inline const Color BorderHi  = Color::RGB(58,  58,  84);   // #3a3a54  active border

// ── Text ─────────────────────────────────────────────
inline const Color Text      = Color::RGB(224, 223, 240);  // #e0dff0  primary text
inline const Color TextSub   = Color::RGB(136, 136, 152);  // #888898  secondary text
inline const Color TextDim   = Color::RGB(85,  85,  104);  // #555568  muted/metadata

// ── Pastel Accents ────────────────────────────────────
inline const Color Rose      = Color::RGB(244, 167, 185);  // errors / failure
inline const Color RoseDim   = Color::RGB(196, 123, 142);
inline const Color Peach     = Color::RGB(248, 180, 160);  // launcher / inputs
inline const Color Gold      = Color::RGB(245, 214, 160);  // warnings / slow builds
inline const Color Sage      = Color::RGB(168, 216, 176);  // success / passing
inline const Color SageDim   = Color::RGB(110, 168, 120);
inline const Color Sky       = Color::RGB(160, 200, 240);  // active / running
inline const Color SkyDim    = Color::RGB(106, 158, 200);
inline const Color Lavender  = Color::RGB(196, 176, 240);  // interactive / keybinds
inline const Color LavDim    = Color::RGB(138, 120, 192);
inline const Color Mint      = Color::RGB(160, 232, 216);  // flamechart / perf
inline const Color MintDim   = Color::RGB(96,  184, 160);

// ── Semantic aliases (use these in business logic) ───
inline const Color ColorRunning = Sky;
inline const Color ColorSuccess = Sage;
inline const Color ColorFailed  = Rose;
inline const Color ColorWarning = Gold;
inline const Color ColorPending = Gold;
inline const Color ColorKeybind = Lavender;

// ─────────────────────────────────────────────────────
//  Helpers: produce styled Elements quickly
// ─────────────────────────────────────────────────────

// Dim text span
inline Element dim(const std::string& s) {
    return text(s) | color(TextDim);
}

// Subtext span
inline Element sub(const std::string& s) {
    return text(s) | color(TextSub);
}

// Accent text
inline Element accent(const std::string& s, Color c) {
    return text(s) | color(c);
}

// Bold accent
inline Element bold_accent(const std::string& s, Color c) {
    return text(s) | color(c) | bold;
}

// Keybind hint: e.g. "[q] quit"
// format: [key] desc
inline Element keyhint(const std::string& key, const std::string& desc) {
    return hbox(Elements{
        text("[")  | color(TextDim),
        text(key)  | color(Lavender) | bold,
        text("] ") | color(TextDim),
        text(desc) | color(TextSub),
    });
}

// ─────────────────────────────────────────────────────
//  Pill badge — proper borderRounded with matching color
//  text(" LABEL ") | color(c) | bold | borderRounded | color(c)
// ─────────────────────────────────────────────────────
inline Element Badge(const std::string& label, Color c) {
    return hbox(Elements{
        text(" "),
        text(label) | color(c) | bold,
        text(" "),
    }) | borderRounded | color(c);
}

// ─────────────────────────────────────────────────────
//  Left-accent build card border strip
//  Single bold "┃" character in the semantic color
// ─────────────────────────────────────────────────────
inline Element AccentStrip(Color c) {
    return text("┃") | color(c) | bold;
}

// ─────────────────────────────────────────────────────
//  Horizontal rule separator
// ─────────────────────────────────────────────────────
inline Element Rule() {
    return separator() | color(Border);
}

// ─────────────────────────────────────────────────────
//  Progress bar — always 1 row tall
//  fill: 0.0 → 1.0
// ─────────────────────────────────────────────────────
inline Element ProgressBar(float fill, Color fill_color) {
    return gauge(fill) | color(fill_color) | size(HEIGHT, EQUAL, 1);
}

// ─────────────────────────────────────────────────────
//  RoundedPanel — every panel uses this for consistent
//  borderRounded + colored title look
// ─────────────────────────────────────────────────────
inline Element RoundedPanel(const std::string& icon,
                             const std::string& title,
                             Color              accent_color,
                             Element            body) {
    auto header = hbox(Elements{
        text(" " + icon + " ") | color(accent_color),
        text(title)            | color(accent_color) | bold,
        text(" "),
    });
    return window(header, body | flex) | borderRounded | color(BorderHi);
}

} // namespace Theme
