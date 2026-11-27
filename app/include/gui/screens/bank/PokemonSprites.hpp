#pragma once

#include "gui/GfxResources.hpp"
#include "save/pokemon/PokemonData.hpp"

#include <citro2d.h>

#include <optional>
#include <utility>

class PokemonSprites {
public:
    explicit PokemonSprites(const GfxResources& resources) : resources_(resources) {}

    void draw(const PokemonSummary& summary, float cx, float cy, float z, float badgeZ, bool dimmed,
              float xShift = 0.0F) const;
    void drawCarried(const PokemonSummary& summary, float cx, float cy, float z) const;

private:
    std::optional<C2D_Image> spriteOf(const PokemonSummary& summary) const;
    std::pair<float, float> spriteSize(const std::optional<C2D_Image>& sprite) const;
    void drawBody(const std::optional<C2D_Image>& sprite, float cx, float cy, float z, float width, float height,
                  const C2D_ImageTint* tint) const;

    const GfxResources& resources_;
};
