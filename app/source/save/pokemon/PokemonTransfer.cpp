#include "save/pokemon/PokemonTransfer.hpp"

#include <pkx/PK2.hpp>
#include <pkx/PK4.hpp>
#include <pkx/PK5.hpp>
#include <pkx/PK6.hpp>
#include <pkx/PK7.hpp>
#include <pkx/PKX.hpp>
#include <sav/Sav.hpp>
#include <utils/VersionTables.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace PokemonTransfer {

namespace {
constexpr std::array<pksm::Generation, 8> GenerationsByFormat{
    pksm::Generation::UNUSED, pksm::Generation::ONE, pksm::Generation::TWO, pksm::Generation::THREE,
    pksm::Generation::FOUR, pksm::Generation::FIVE, pksm::Generation::SIX, pksm::Generation::SEVEN
};
}

std::uint8_t pokemonFormat(pksm::Generation generation) {
    for (std::size_t format = 1; format < GenerationsByFormat.size(); ++format) {
        if (GenerationsByFormat[format] == generation) {
            return static_cast<std::uint8_t>(format);
        }
    }
    return 0;
}

pksm::Generation generationFromFormat(std::uint8_t format) {
    return format < GenerationsByFormat.size() ? GenerationsByFormat[format] : pksm::Generation::UNUSED;
}

pksm::Generation expectedGeneration(PokemonFormat format) {
    return generationFromFormat(static_cast<std::uint8_t>(format));
}

bool canConvert(std::uint8_t sourceFormat, std::uint8_t targetFormat) {
    if (generationFromFormat(sourceFormat) == pksm::Generation::UNUSED
        || generationFromFormat(targetFormat) == pksm::Generation::UNUSED) {
        return false;
    }
    if (sourceFormat == targetFormat) {
        return true;
    }
    if (sourceFormat > targetFormat) {
        return false;
    }
    // Poke Transporter takes the Virtual Console games to Gen 7 only.
    return sourceFormat > 2 || targetFormat <= 2 || targetFormat == 7;
}

std::unique_ptr<pksm::PKX> convertForSave(
    const pksm::PKX& source,
    std::uint8_t sourceFormat,
    std::uint8_t targetGeneration,
    pksm::Sav& save
) {
    if (sourceFormat == targetGeneration || !canConvert(sourceFormat, targetGeneration)) {
        return nullptr;
    }
    std::unique_ptr<pksm::PKX> converted;
    switch (targetGeneration) {
        case 2: converted = source.convertToG2(save); break;
        case 4: converted = source.convertToG4(save); break;
        case 5: converted = source.convertToG5(save); break;
        case 6: converted = source.convertToG6(save); break;
        case 7: converted = source.convertToG7(save); break;
        default: return nullptr;
    }
    if (converted && !converted->nicknamed()) {
        converted->nickname(converted->species().localize(converted->language()));
        converted->refreshChecksum();
    }
    return converted;
}

bool checksumValid(const pksm::PKX& pokemon) {
    const std::unique_ptr<pksm::PKX> copy = pokemon.clone();
    copy->refreshChecksum();
    return copy->checksum() == pokemon.checksum();
}

namespace {
std::array<pksm::Move, 4> movesOf(const pksm::PKX& pokemon) {
    return {pokemon.move(0), pokemon.move(1), pokemon.move(2), pokemon.move(3)};
}
}

bool fitsGame(const pksm::PKX& pokemon, pksm::GameVersion game) {
    namespace VersionTables = pksm::VersionTables;
    const pksm::Species species = pokemon.species();
    if (!VersionTables::availableSpecies(game).contains(species)
        || pokemon.alternativeForm() >= VersionTables::formCount(game, species)) {
        return false;
    }
    for (const pksm::Move move : movesOf(pokemon)) {
        if (move != pksm::Move::None && !VersionTables::availableMoves(game).contains(move)) {
            return false;
        }
    }
    const pksm::Generation generation = pokemon.generation();
    // Gen 1 has no held item; Gen 1 and 2 have no ability or ball.
    if (generation == pksm::Generation::ONE) {
        return true;
    }
    if (generation == pksm::Generation::TWO) {
        // The Gen 2 tables hold the game's own item indexes, not the national ones heldItem() gives.
        const int item = static_cast<const pksm::PK2&>(pokemon).heldItem2();
        return item == 0 || VersionTables::availableItems(game).contains(item);
    }
    if (pokemon.heldItem() != 0 && !VersionTables::availableItems(game).contains(pokemon.heldItem())) {
        return false;
    }
    return VersionTables::availableAbilities(game).contains(pokemon.ability())
        && VersionTables::availableBalls(game).contains(pokemon.ball());
}

namespace {
ImportLoss lossOf(const pksm::PKX& source, const pksm::PKX& converted) {
    ImportLoss loss;
    loss.heldItem = source.generation() != pksm::Generation::ONE && source.heldItem() != 0
        && converted.heldItem() == 0;
    const std::array<pksm::Move, 4> kept = movesOf(converted);
    for (const pksm::Move move : movesOf(source)) {
        loss.moves = loss.moves || (move != pksm::Move::None && std::find(kept.begin(), kept.end(), move) == kept.end());
    }
    return loss;
}

PreparedPokemon refused(ImportRefusal refusal) {
    return {{refusal, {}}, nullptr};
}
}

PreparedPokemon prepareForSave(const PokemonPayload& payload, pksm::Sav& save) {
    const std::uint8_t target = pokemonFormat(save.generation());
    const pksm::Generation generation = generationFromFormat(payload.format);
    if (!payload.known() || generation == pksm::Generation::UNUSED) {
        return refused(ImportRefusal::Corrupted);
    }
    if (!canConvert(payload.format, target)) {
        return refused(ImportRefusal::Generation);
    }
    std::vector<std::uint8_t> buffer = payload.data;
    std::unique_ptr<pksm::PKX> source = pksm::PKX::getPKM(generation, buffer.data(), buffer.size(), false);
    if (!source || static_cast<std::uint16_t>(source->species()) == 0 || !checksumValid(*source)) {
        return refused(ImportRefusal::Corrupted);
    }
    PreparedPokemon result;
    if (payload.format == target) {
        result.pokemon = std::move(source);
    } else {
        if (source->egg()) {
            return refused(ImportRefusal::Egg);
        }
        // Cosplay Pikachu forms would become Cap Pikachu in Gen 7; Poke Transporter refuses them.
        if (source->generation() == pksm::Generation::SIX && source->species() == pksm::Species::Pikachu
            && source->alternativeForm() != 0) {
            return refused(ImportRefusal::NotInGame);
        }
        result.pokemon = convertForSave(*source, payload.format, target, save);
        if (!result.pokemon) {
            return refused(ImportRefusal::NotInGame);
        }
        result.check.loss = lossOf(*source, *result.pokemon);
    }
    if (!fitsGame(*result.pokemon, save.version())) {
        return refused(ImportRefusal::NotInGame);
    }
    return result;
}

}
