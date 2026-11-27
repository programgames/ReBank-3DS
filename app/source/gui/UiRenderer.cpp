#include "gui/UiRenderer.hpp"

#include "gui/TextLayout.hpp"
#include "gui/widgets/Shapes.hpp"
#include "platform/Clock.hpp"
#include "platform/Logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace Gui;

namespace {
constexpr float TextDepth = 0.85F;

std::string sanitizeUtf8(std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    std::size_t index = 0;
    while (index < value.size()) {
        const std::size_t length = TextLayout::codePointLength(value, index);
        if (length == 1 && static_cast<unsigned char>(value[index]) >= 0x80) {
            sanitized.push_back('?');
            ++index;
            continue;
        }
        sanitized.append(value.substr(index, length));
        index += length;
    }
    return sanitized;
}
}

namespace {
u32 decodeCodePoint(std::string_view value, std::size_t index, std::size_t length) {
    const auto lead = static_cast<unsigned char>(value[index]);
    u32 codePoint = length == 1 ? lead : lead & (0xFF >> (length + 1));
    for (std::size_t extra = 1; extra < length; ++extra) {
        codePoint = (codePoint << 6) | (static_cast<unsigned char>(value[index + extra]) & 0x3F);
    }
    return codePoint;
}

bool hangul(u32 codePoint) {
    return (codePoint >= 0xAC00 && codePoint <= 0xD7A3) || (codePoint >= 0x1100 && codePoint <= 0x11FF)
        || (codePoint >= 0x3130 && codePoint <= 0x318F);
}

// Fallback fonts, in the slot order of UiRenderer::fallbacks_.
constexpr std::array<CFG_Region, 3> FallbackRegions{CFG_REGION_KOR, CFG_REGION_CHN, CFG_REGION_TWN};
constexpr std::array<std::size_t, 3> HangulFontOrder{0, 1, 2}; // KOR first
constexpr std::array<std::size_t, 3> HanziFontOrder{1, 2, 0};  // CHN, then TWN

// Only these ranges can come from the KOR/CHN/TWN fonts; any other missing glyph must not load them.
bool cjk(u32 codePoint) {
    return hangul(codePoint) || (codePoint >= 0x2E80 && codePoint <= 0x9FFF) || (codePoint >= 0xF900 && codePoint <= 0xFAFF)
        || (codePoint >= 0xFF00 && codePoint <= 0xFFEF);
}

bool hasGlyph(C2D_Font font, u32 codePoint) {
    const FINF_s* info = C2D_FontGetInfo(font);
    return !info || C2D_FontGlyphIndexFromCodePoint(font, codePoint) != info->alterCharIndex;
}
}

// App declares ui_ after resources_, so this runs before ~GfxResources calls C2D_Fini.
UiRenderer::~UiRenderer() {
    for (C2D_Font font : fallbacks_) {
        if (font) {
            C2D_FontFree(font);
        }
    }
}

C2D_Font UiRenderer::fallback(std::size_t index) const {
    if (!fallbackTried_[index]) {
        fallbackTried_[index] = true;
        fallbacks_[index] = C2D_FontLoadSystem(FallbackRegions[index]);
        if (fallbacks_[index]) {
            C2D_FontSetFilter(fallbacks_[index], GPU_NEAREST, GPU_LINEAR);
        } else {
            Logger::instance().warning("Fallback system font " + std::to_string(static_cast<int>(FallbackRegions[index]))
                                       + " could not be loaded");
        }
    }
    return fallbacks_[index];
}

C2D_Font UiRenderer::fontFor(u32 codePoint) const {
    if (!cjk(codePoint) || hasGlyph(font_, codePoint)) {
        return font_;
    }
    // Korean, Chinese and Taiwanese consoles ship glyphs the USA/EUR/JPN font lacks: OT names and
    // nicknames from those games. The fonts load on the first glyph that needs them.
    for (const std::size_t index : hangul(codePoint) ? HangulFontOrder : HanziFontOrder) {
        if (const C2D_Font font = fallback(index); font && hasGlyph(font, codePoint)) {
            return font;
        }
    }
    return font_;
}

std::vector<UiRenderer::FontRun> UiRenderer::fontRuns(std::string_view value) const {
    std::vector<FontRun> runs;
    std::size_t index = 0;
    while (index < value.size()) {
        const std::size_t length = TextLayout::codePointLength(value, index);
        const C2D_Font font = fontFor(decodeCodePoint(value, index, length));
        if (runs.empty() || runs.back().font != font) {
            runs.push_back({font, value.substr(index, length)});
        } else {
            runs.back().text = value.substr(runs.back().text.data() - value.data(), runs.back().text.size() + length);
        }
        index += length;
    }
    return runs;
}

C2D_Text UiRenderer::parse(std::string_view value, C2D_Font font) {
    C2D_DrawRectSolid(0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0);
    C2D_Text text;
    C2D_TextFontParse(&text, font, activeBuffer_, std::string(value).c_str());
    return text;
}

float UiRenderer::shiftFor(Anchor anchor, float width) {
    return anchor == Anchor::Center ? width * 0.5F : (anchor == Anchor::Right ? width : 0.0F);
}

void UiRenderer::draw(std::string_view value, float x, float y, float size, u32 color, Anchor anchor) {
    const std::string sanitized = sanitizeUtf8(value);
    const std::vector<FontRun> runs = fontRuns(sanitized);
    if (runs.size() <= 1) {
        C2D_Text text = parse(sanitized, runs.empty() ? font_ : runs.front().font);
        float width = 0.0F;
        if (anchor != Anchor::Left) {
            C2D_TextGetDimensions(&text, size, size, &width, nullptr);
        }
        C2D_DrawText(&text, C2D_WithColor, std::round(x - shiftFor(anchor, width)), std::round(y), TextDepth, size, size, color);
        return;
    }
    // ponytail: mixed-font text is laid out as one line; a line break only works in the single-font path.
    const float width = measure(sanitized, size);
    float cursor = std::round(x - shiftFor(anchor, width));
    for (const FontRun& run : runs) {
        C2D_Text text = parse(run.text, run.font);
        C2D_DrawText(&text, C2D_WithColor, std::round(cursor), std::round(y), TextDepth, size, size, color);
        cursor += measure(run.text, size);
    }
}

void UiRenderer::drawText(std::string_view value, float x, float y, float size, u32 color) {
    draw(value, x, y, size, color, Anchor::Left);
}

void UiRenderer::drawCentered(std::string_view value, float centerX, float y, float size, u32 color) {
    draw(value, centerX, y, size, color, Anchor::Center);
}

void UiRenderer::drawRight(std::string_view value, float rightX, float y, float size, u32 color) {
    draw(value, rightX, y, size, color, Anchor::Right);
}

float UiRenderer::measure(std::string_view value, float size) const {
    float width = 0.0F;
    std::size_t index = 0;
    while (index < value.size()) {
        const std::size_t length = TextLayout::codePointLength(value, index);
        const u32 codePoint = decodeCodePoint(value, index, length);
        index += length;
        const C2D_Font font = fontFor(codePoint);
        const charWidthInfo_s* info = C2D_FontGetCharWidthInfo(font, C2D_FontGlyphIndexFromCodePoint(font, codePoint));
        width += info ? info->charWidth * size : 0.0F;
    }
    return width;
}

std::vector<std::string> UiRenderer::wrap(std::string_view value, float size, float width, std::size_t maxLines) const {
    return TextLayout::wrap(value, width, [this, size](std::string_view text) { return measure(text, size); }, maxLines);
}

std::string UiRenderer::fit(std::string_view value, float size, float width) const {
    return TextLayout::fitWithEllipsis(value, width, [this, size](std::string_view text) { return measure(text, size); });
}

float UiRenderer::lineHeight(float size) const {
    const FINF_s* info = C2D_FontGetInfo(font_);
    return (info ? info->lineFeed : 30.0F) * size;
}

float UiRenderer::textWidth(std::string_view value, float size) {
    return measure(sanitizeUtf8(value), size);
}

void UiRenderer::drawWarmBackground() {
    const u32 bottom = C2D_Color32(255, 228, 204, 255);
    C2D_DrawRectangle(0.0F, 0.0F, 0.01F, 320.0F, 240.0F, WarmBackground, WarmBackground, bottom, bottom);
}

void UiRenderer::drawWarmTop(float eyeOffset) {
    const float seconds = static_cast<float>(Clock::seconds());
    const float breathe = 0.5F + 0.5F * std::sin(seconds * 0.9F);
    C2D_DrawRectangle(0.0F, 0.0F, 0.01F, 400.0F, 240.0F, WarmGlowTop, WarmGlowTop, WarmGlowBottom, WarmGlowBottom);
    for (int ring = 0; ring < 4; ++ring) {
        C2D_DrawCircleSolid(200.0F + eyeOffset * 0.3F, 92.0F, 0.02F, 120.0F - ring * 24.0F,
                            C2D_Color32(255, 226, 184, static_cast<u8>(18 + breathe * 10.0F)));
    }
}

void UiRenderer::drawSpinner(float centerX, float centerY, float radius, u32 color) {
    constexpr int Segments = 12;
    const float seconds = static_cast<float>(Clock::seconds());
    const float head = seconds * 6.0F;
    const float dot = std::max(2.0F, radius * 0.22F);
    for (int index = 0; index < Segments; ++index) {
        const float phase = head - static_cast<float>(index) * 0.42F;
        const float fade = 1.0F - static_cast<float>(index) / Segments;
        const u32 tint = (color & 0x00FFFFFFU) | (static_cast<u32>(fade * fade * 255.0F) << 24);
        C2D_DrawCircleSolid(centerX + std::cos(phase) * radius, centerY + std::sin(phase) * radius, 0.3F,
                            dot * (0.45F + 0.55F * fade), tint);
    }
}

void UiRenderer::drawProgressBar(const UiRect& rect, float share, u32 track) {
    Gui::drawRoundedRect(rect, ControlRadius, 0.2F, track);
    const float width = rect.width * std::clamp(share, 0.0F, 1.0F);
    if (width < rect.height) {
        return;
    }
    Gui::drawRoundedRect({rect.x, rect.y, width, rect.height}, ControlRadius, 0.3F, WarmBrand);
    Gui::drawRoundedRect({rect.x + 2.0F, rect.y + 1.5F, width - 4.0F, rect.height * 0.35F}, ControlRadius, 0.31F, C2D_Color32(255, 190, 130, 160));
}

void UiRenderer::drawBusyBar(const UiRect& rect, u32 track) {
    constexpr float BlockWidth = 60.0F;
    const float cycle = std::fmod(static_cast<float>(Clock::seconds()) * 0.65F, 1.0F);
    Gui::drawRoundedRect(rect, ControlRadius, 0.2F, track);
    Gui::drawRoundedRect({rect.x + cycle * (rect.width - BlockWidth), rect.y, BlockWidth, rect.height}, ControlRadius, 0.3F,
                         WarmBrand);
}

void UiRenderer::drawCheckMark(float centerX, float centerY, float radius) {
    C2D_DrawCircleSolid(centerX, centerY + 2.0F, 0.29F, radius, WarmBrandDark);
    C2D_DrawCircleSolid(centerX, centerY, 0.3F, radius, Surface);
    C2D_DrawCircleSolid(centerX, centerY, 0.31F, radius - 4.0F, WarmBrand);
    const float s = radius / 26.0F;
    C2D_DrawLine(centerX - 12.0F * s, centerY + 1.0F * s, Surface, centerX - 3.0F * s, centerY + 10.0F * s, Surface, 5.0F * s, 0.4F);
    C2D_DrawLine(centerX - 3.0F * s, centerY + 10.0F * s, Surface, centerX + 13.0F * s, centerY - 9.0F * s, Surface, 5.0F * s, 0.4F);
}

void UiRenderer::drawButton(const UiRect& rect, std::string_view label, bool primary) {
    Gui::drawRoundedRect({rect.x, rect.y + 3.0F, rect.width, rect.height}, ControlRadius, 0.09F,
             primary ? WarmBrandDark : C2D_Color32(234, 200, 176, 255));
    Gui::drawRoundedRect(rect, ControlRadius, 0.1F, primary ? WarmBrand : Surface);
    drawCentered(label, rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F - 9.0F, 0.58F,
                 primary ? Surface : WarmInk);
}

void UiRenderer::drawField(const UiRect& rect, std::string_view label, const std::string& value, bool password) {
    Gui::drawRoundedRect({rect.x, rect.y + 2.0F, rect.width, rect.height}, ControlRadius, 0.09F, C2D_Color32(234, 200, 176, 255));
    Gui::drawRoundedRect(rect, ControlRadius, 0.1F, Surface);
    drawText(label, rect.x + 12.0F, rect.y + 5.0F, 0.42F, WarmMuted);
    drawText(password ? std::string(value.size(), '*') : value, rect.x + 12.0F, rect.y + 21.0F, 0.52F, WarmInk);
}
