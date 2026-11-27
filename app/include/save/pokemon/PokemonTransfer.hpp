#pragma once

#include "save/catalog/GameCatalog.hpp"
#include "save/pokemon/PokemonData.hpp"

#include <cstdint>
#include <memory>

namespace pksm {
class PKX;
class Sav;
class Generation;
}

namespace PokemonTransfer {
pksm::Generation expectedGeneration(PokemonFormat format);
std::uint8_t pokemonFormat(pksm::Generation generation);

pksm::Generation generationFromFormat(std::uint8_t format);

// Official rules: no move to an older format, and Gen 1/2 Pokemon reach the 3DS games only in Gen 7.
bool canConvert(std::uint8_t sourceFormat, std::uint8_t targetFormat);

std::unique_ptr<pksm::PKX> convertForSave(
    const pksm::PKX& source,
    std::uint8_t sourceFormat,
    std::uint8_t targetGeneration,
    pksm::Sav& save
);

enum class ImportRefusal : std::uint8_t {
    None,
    Generation, // the format cannot reach the save's generation
    Egg,        // an egg cannot change generation
    Corrupted,  // unreadable payload or stored checksum that does not match the data
    NotInGame,  // the target game has no such species, form, move, item, ability or ball
};

// What the conversion to an older-style format drops.
struct ImportLoss {
    bool heldItem = false;
    bool moves = false;

    bool any() const { return heldItem || moves; }
};

struct ImportCheck {
    ImportRefusal refusal = ImportRefusal::None;
    ImportLoss loss;

    bool allowed() const { return refusal == ImportRefusal::None; }
};

struct PreparedPokemon {
    ImportCheck check;
    // Set only when check.allowed(): the Pokemon in the save's format.
    std::unique_ptr<pksm::PKX> pokemon;
};

bool checksumValid(const pksm::PKX& pokemon);

// True when the game knows every value of the Pokemon: species, form, moves, held item, ability, ball.
bool fitsGame(const pksm::PKX& pokemon, pksm::GameVersion game);

// Reads the payload, refuses what the save's game cannot hold, and converts the rest to the save's format.
PreparedPokemon prepareForSave(const PokemonPayload& payload, pksm::Sav& save);
}
