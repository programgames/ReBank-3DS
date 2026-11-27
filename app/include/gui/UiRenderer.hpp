#pragma once

#include "gui/Theme.hpp"

#include <citro2d.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

class UiRenderer {
public:
    UiRenderer() = default;
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    void setFont(C2D_Font font) { font_ = font; }
    void setActiveBuffer(C2D_TextBuf buffer) { activeBuffer_ = buffer; }

    void drawText(std::string_view value, float x, float y, float size, u32 color);
    void drawCentered(std::string_view value, float centerX, float y, float size, u32 color);
    void drawRight(std::string_view value, float rightX, float y, float size, u32 color);
    float textWidth(std::string_view value, float size);
    float measure(std::string_view value, float size) const;
    float lineHeight(float size) const;
    std::vector<std::string> wrap(std::string_view value, float size, float width,
                                  std::size_t maxLines = std::numeric_limits<std::size_t>::max()) const;
    std::string fit(std::string_view value, float size, float width) const;
    void drawWarmBackground();
    void drawWarmTop(float eyeOffset);
    void drawSpinner(float centerX, float centerY, float radius, u32 color);
    void drawProgressBar(const UiRect& rect, float share, u32 track);
    void drawBusyBar(const UiRect& rect, u32 track);
    void drawCheckMark(float centerX, float centerY, float radius);
    void drawButton(const UiRect& rect, std::string_view label, bool primary);
    void drawField(const UiRect& rect, std::string_view label, const std::string& value, bool password);

private:
    enum class Anchor { Left, Center, Right };

    struct FontRun {
        C2D_Font font;
        std::string_view text;
    };
    static constexpr std::size_t FallbackCount = 3;
    static constexpr std::array<CFG_Region, FallbackCount> FallbackRegions{CFG_REGION_KOR, CFG_REGION_CHN,
                                                                           CFG_REGION_TWN};

    C2D_Text parse(std::string_view value, C2D_Font font);
    C2D_Font fallback(std::size_t index) const;
    C2D_Font fontFor(u32 codePoint) const;
    std::vector<FontRun> fontRuns(std::string_view value) const;
    void draw(std::string_view value, float x, float y, float size, u32 color, Anchor anchor);

    C2D_Font font_ = nullptr;
    C2D_TextBuf activeBuffer_ = nullptr;
    mutable std::array<C2D_Font, FallbackCount> fallbacks_{};
    mutable std::array<bool, FallbackCount> fallbackTried_{};
};
