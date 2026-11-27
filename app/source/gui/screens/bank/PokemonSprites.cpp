#include "gui/screens/bank/PokemonSprites.hpp"

#include "gui/widgets/PokemonBadges.hpp"

#include <algorithm>
#include <cmath>

using namespace Gui;

namespace {
constexpr u32 DimTint = C2D_Color32(72, 72, 72, 255);
constexpr u32 ShadowColor = C2D_Color32(20, 20, 20, 90);
constexpr float CarryLift = 8.0F;
constexpr float PlaceholderSize = 32.0F;
constexpr u32 PlaceholderColor = C2D_Color32(150, 150, 150, 255);
constexpr u32 PlaceholderDimColor = C2D_Color32(90, 90, 90, 255);
constexpr u32 PlaceholderCoreColor = C2D_Color32(235, 235, 235, 255);
}

std::optional<C2D_Image> PokemonSprites::spriteOf(const PokemonSummary& summary) const {
    if (!summary.occupied() || !resources_.pokemonSprites
        || summary.species >= C2D_SpriteSheetCount(resources_.pokemonSprites)) {
        return std::nullopt;
    }
    return C2D_SpriteSheetGetImage(resources_.pokemonSprites, summary.species);
}

// Size of a regular sprite, so a placeholder and its badges sit where a real sprite would.
std::pair<float, float> PokemonSprites::spriteSize(const std::optional<C2D_Image>& sprite) const {
    if (sprite) {
        return {static_cast<float>(sprite->subtex->width), static_cast<float>(sprite->subtex->height)};
    }
    if (resources_.pokemonSprites && C2D_SpriteSheetCount(resources_.pokemonSprites) > 0) {
        const C2D_Image first = C2D_SpriteSheetGetImage(resources_.pokemonSprites, 0);
        return {static_cast<float>(first.subtex->width), static_cast<float>(first.subtex->height)};
    }
    return {PlaceholderSize, PlaceholderSize};
}

// An occupied slot whose species has no sprite (outside the sheet, or the sheet failed to load) must
// never look free: it gets a neutral disc instead.
void PokemonSprites::drawBody(const std::optional<C2D_Image>& sprite, float cx, float cy, float z, float width,
                              float height, const C2D_ImageTint* tint) const {
    if (sprite) {
        C2D_DrawImageAt(*sprite, std::round(cx - width * 0.5F), std::round(cy - height * 0.5F), z, tint);
        return;
    }
    const float radius = std::min(width, height) * 0.32F;
    C2D_DrawCircleSolid(cx, cy, z, radius, tint ? PlaceholderDimColor : PlaceholderColor);
    C2D_DrawCircleSolid(cx, cy, z + 0.001F, radius * 0.4F, PlaceholderCoreColor);
}

void PokemonSprites::draw(const PokemonSummary& summary, float cx, float cy, float z, float badgeZ, bool dimmed,
                          float xShift) const {
    if (!summary.occupied()) {
        return;
    }
    const std::optional<C2D_Image> sprite = spriteOf(summary);
    const auto [width, height] = spriteSize(sprite);
    C2D_ImageTint tint{};
    if (dimmed) {
        C2D_PlainImageTint(&tint, DimTint, 0.82F);
    }
    drawBody(sprite, cx + xShift, cy, z, width, height, dimmed ? &tint : nullptr);
    drawPokemonBadges(resources_.overlayIcons, summary, cx, cy, width * 0.5F, height * 0.5F, badgeZ);
}

void PokemonSprites::drawCarried(const PokemonSummary& summary, float cx, float cy, float z) const {
    if (!summary.occupied()) {
        return;
    }
    const std::optional<C2D_Image> sprite = spriteOf(summary);
    const auto [width, height] = spriteSize(sprite);
    const float shadowWidth = width * 0.6F;
    const float shadowHeight = shadowWidth * 0.35F;
    C2D_DrawEllipseSolid(cx - shadowWidth * 0.5F, cy + height * 0.42F - shadowHeight * 0.5F, z - 0.02F, shadowWidth,
                         shadowHeight, ShadowColor);
    const float spriteY = cy - CarryLift;
    drawBody(sprite, cx, spriteY, z, width, height, nullptr);
    drawPokemonBadges(resources_.overlayIcons, summary, cx, spriteY, width * 0.5F, height * 0.5F, z + 0.01F);
}
