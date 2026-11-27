// Host-side tests for the cross-game rules in PokemonTransfer, against the real PKSM-Core tables.
// Saves are built on zero-filled images: enough for the version, the conversions and the trade hooks.

#include "save/pokemon/PokemonTransfer.hpp"

#include <pkx/PK1.hpp>
#include <pkx/PK2.hpp>
#include <pkx/PK4.hpp>
#include <pkx/PK5.hpp>
#include <pkx/PK6.hpp>
#include <pkx/PK7.hpp>
#include <sav/SavB2W2.hpp>
#include <sav/SavBW.hpp>
#include <sav/SavHGSS.hpp>
#include <sav/SavUSUM.hpp>
#include <utils/VersionTables.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::printf("FAIL  %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        }                                                                       \
    } while (false)

using namespace pksm;
using PokemonTransfer::ImportRefusal;

template <Generation::EnumType G>
auto pokemon(Species species, u16 form = 0) {
    using Pkm = typename GenToPkx<G>::PKX;
    std::unique_ptr<Pkm> pkm;
    if constexpr (G == Generation::ONE || G == Generation::TWO) {
        pkm = PKX::getPKM<G>(nullptr, Pkm::INT_LENGTH_WITH_NAMES);
    } else {
        pkm = PKX::getPKM<G>(nullptr, Pkm::BOX_LENGTH);
        pkm->ability(Ability::Static);
        pkm->ball(Ball::Poke);
    }
    pkm->species(species);
    pkm->alternativeForm(form);
    pkm->move(0, Move::Tackle);
    pkm->refreshChecksum();
    return pkm;
}

PokemonPayload payloadOf(const PKX& pkm) {
    const auto raw = pkm.rawData();
    return {PokemonTransfer::pokemonFormat(pkm.generation()), std::vector<std::uint8_t>(raw.begin(), raw.end())};
}

template <typename Save>
std::unique_ptr<Save> blankSave(std::size_t size) {
    return std::make_unique<Save>(std::shared_ptr<u8[]>(new u8[size]()));
}

std::unique_ptr<PKX> withMove(std::unique_ptr<PKX> pkm, Move move) {
    pkm->move(1, move);
    return pkm;
}

std::unique_ptr<PKX> withItem(std::unique_ptr<PKX> pkm, u16 item) {
    pkm->heldItem(item);
    return pkm;
}

// One item that `newer` knows and `older` does not.
int itemOnlyIn(GameVersion newer, GameVersion older) {
    for (int item : VersionTables::availableItems(newer)) {
        if (!VersionTables::availableItems(older).contains(item)) {
            return item;
        }
    }
    return 0;
}

void canConvertFollowsTheOfficialChain() {
    using PokemonTransfer::canConvert;
    CHECK(canConvert(1, 1) && canConvert(1, 2) && canConvert(1, 7) && canConvert(2, 7));
    CHECK(!canConvert(1, 4) && !canConvert(1, 5) && !canConvert(1, 6));
    CHECK(!canConvert(2, 4) && !canConvert(2, 5) && !canConvert(2, 6) && !canConvert(2, 1));
    CHECK(canConvert(4, 5) && canConvert(4, 7) && canConvert(5, 6) && canConvert(6, 7));
    CHECK(!canConvert(7, 6) && !canConvert(5, 4) && !canConvert(0, 7) && !canConvert(7, 8));
}

void usumOnlyValuesStayOutOfSunMoon() {
    using PokemonTransfer::fitsGame;
    const GameVersion sm = GameVersion::SN;
    const GameVersion usum = GameVersion::US;
    CHECK(fitsGame(*pokemon<Generation::SEVEN>(Species::Pikachu), sm));
    for (u16 species = 803; species <= 807; ++species) {
        CHECK(fitsGame(*pokemon<Generation::SEVEN>(Species(species)), usum));
        CHECK(!fitsGame(*pokemon<Generation::SEVEN>(Species(species)), sm));
    }
    for (u16 move = 721; move <= u16(VersionTables::maxMove(usum)); ++move) {
        CHECK(fitsGame(*withMove(pokemon<Generation::SEVEN>(Species::Pikachu), Move(move)), usum));
        CHECK(!fitsGame(*withMove(pokemon<Generation::SEVEN>(Species::Pikachu), Move(move)), sm));
    }
    CHECK(VersionTables::maxItem(usum) > 921);
    CHECK(!fitsGame(*withItem(pokemon<Generation::SEVEN>(Species::Pikachu), 921), sm));
    CHECK(!fitsGame(*withItem(pokemon<Generation::SEVEN>(Species::Pikachu), VersionTables::maxItem(usum)), sm));
    CHECK(fitsGame(*pokemon<Generation::SEVEN>(Species::Lycanroc, 1), sm));
    CHECK(fitsGame(*pokemon<Generation::SEVEN>(Species::Lycanroc, 2), usum));
    CHECK(!fitsGame(*pokemon<Generation::SEVEN>(Species::Lycanroc, 2), sm));
}

void orasOnlyValuesStayOutOfXY() {
    using PokemonTransfer::fitsGame;
    const GameVersion xy = GameVersion::X;
    const GameVersion oras = GameVersion::OR;
    CHECK(fitsGame(*pokemon<Generation::SIX>(Species::Pikachu), xy));
    for (u16 move = 618; move <= 621; ++move) {
        CHECK(fitsGame(*withMove(pokemon<Generation::SIX>(Species::Pikachu), Move(move)), oras));
        CHECK(!fitsGame(*withMove(pokemon<Generation::SIX>(Species::Pikachu), Move(move)), xy));
    }
    const int megaStone = itemOnlyIn(oras, xy);
    CHECK(megaStone != 0);
    CHECK(fitsGame(*withItem(pokemon<Generation::SIX>(Species::Pikachu), u16(megaStone)), oras));
    CHECK(!fitsGame(*withItem(pokemon<Generation::SIX>(Species::Pikachu), u16(megaStone)), xy));
    CHECK(fitsGame(*pokemon<Generation::SIX>(Species::Pikachu, 1), oras));
    CHECK(!fitsGame(*pokemon<Generation::SIX>(Species::Pikachu, 1), xy));
}

void b2w2OnlyFormsStayOutOfBW() {
    using PokemonTransfer::fitsGame;
    const GameVersion bw = GameVersion::B;
    const GameVersion b2w2 = GameVersion::B2;
    CHECK(fitsGame(*pokemon<Generation::FIVE>(Species::Kyurem), bw));
    CHECK(fitsGame(*pokemon<Generation::FIVE>(Species::Landorus), bw));
    // Kyurem White and Black, Therian Landorus, Thundurus and Tornadus.
    constexpr std::pair<u16, u16> NewForms[] = {{646, 1}, {646, 2}, {645, 1}, {642, 1}, {641, 1}};
    for (const auto& [species, form] : NewForms) {
        CHECK(fitsGame(*pokemon<Generation::FIVE>(Species(species), form), b2w2));
        CHECK(!fitsGame(*pokemon<Generation::FIVE>(Species(species), form), bw));
    }
}

void platinumOnlyFormsStayOutOfDP() {
    using PokemonTransfer::fitsGame;
    const GameVersion dp = GameVersion::D;
    CHECK(fitsGame(*pokemon<Generation::FOUR>(Species::Giratina), dp));
    CHECK(fitsGame(*pokemon<Generation::FOUR>(Species::Rotom), dp));
    for (const GameVersion newer : std::array<GameVersion, 2>{GameVersion::Pt, GameVersion::HG}) {
        CHECK(fitsGame(*pokemon<Generation::FOUR>(Species::Giratina, 1), newer));
        CHECK(fitsGame(*pokemon<Generation::FOUR>(Species::Rotom, 1), newer));
    }
    CHECK(!fitsGame(*pokemon<Generation::FOUR>(Species::Giratina, 1), dp));
    for (u16 form = 1; form <= 5; ++form) {
        CHECK(!fitsGame(*pokemon<Generation::FOUR>(Species::Rotom, form), dp));
    }
}

void gameBoyPokemonFitTheirGames() {
    using PokemonTransfer::fitsGame;
    CHECK(fitsGame(*pokemon<Generation::ONE>(Species::Mew), GameVersion::RD));
    CHECK(!fitsGame(*pokemon<Generation::ONE>(Species::Chikorita), GameVersion::RD));
    CHECK(fitsGame(*withItem(pokemon<Generation::TWO>(Species::Celebi), 1), GameVersion::C));
    CHECK(!fitsGame(*pokemon<Generation::TWO>(Species::Treecko), GameVersion::GD));
    // Gen 2 items are checked by their Crystal index, not by the national one.
    for (int item : VersionTables::availableItems(GameVersion::C)) {
        auto celebi = pokemon<Generation::TWO>(Species::Celebi);
        celebi->heldItem2(u8(item));
        CHECK(fitsGame(*celebi, GameVersion::C));
    }
}

void conversionReportsDroppedItemAndHmMoves() {
    auto bw = blankSave<SavBW>(0x80000);
    bw->version(GameVersion::B);
    auto pk4 = pokemon<Generation::FOUR>(Species::Pikachu);
    pk4->heldItem(1);
    pk4->move(1, Move::Surf);
    pk4->refreshChecksum();
    const auto prepared = PokemonTransfer::prepareForSave(payloadOf(*pk4), *bw);
    CHECK(prepared.check.allowed());
    CHECK(prepared.check.loss.heldItem && prepared.check.loss.moves);
    CHECK(prepared.pokemon && prepared.pokemon->generation() == Generation::FIVE);

    auto plain = pokemon<Generation::FOUR>(Species::Pikachu);
    const auto clean = PokemonTransfer::prepareForSave(payloadOf(*plain), *bw);
    CHECK(clean.check.allowed() && !clean.check.loss.any());
}

void gameBoyPokemonReachOnlyGen7() {
    auto hgss = blankSave<SavHGSS>(0x80000);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::TWO>(Species::Pikachu)), *hgss).check.refusal
          == ImportRefusal::Generation);
    auto usum = blankSave<SavUSUM>(0x6CC00);
    usum->version(GameVersion::US);
    const auto prepared = PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::ONE>(Species::Pikachu)), *usum);
    CHECK(prepared.check.allowed());
    CHECK(prepared.pokemon && prepared.pokemon->generation() == Generation::SEVEN);
}

void cosplayPikachuStaysInGen6() {
    auto usum = blankSave<SavUSUM>(0x6CC00);
    usum->version(GameVersion::US);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::SIX>(Species::Pikachu, 1)), *usum).check.refusal
          == ImportRefusal::NotInGame);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::SIX>(Species::Pikachu)), *usum).check.allowed());
}

void eggsDoNotChangeGeneration() {
    auto usum = blankSave<SavUSUM>(0x6CC00);
    usum->version(GameVersion::US);
    auto egg = pokemon<Generation::TWO>(Species::Pichu);
    egg->egg(true);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*egg), *usum).check.refusal == ImportRefusal::Egg);

    auto bw = blankSave<SavBW>(0x80000);
    bw->version(GameVersion::B);
    auto egg4 = pokemon<Generation::FOUR>(Species::Pichu);
    egg4->egg(true);
    egg4->refreshChecksum();
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*egg4), *bw).check.refusal == ImportRefusal::Egg);

    auto egg5 = pokemon<Generation::FIVE>(Species::Pichu);
    egg5->egg(true);
    egg5->refreshChecksum();
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*egg5), *bw).check.allowed());
}

void badChecksumIsRefused() {
    auto usum = blankSave<SavUSUM>(0x6CC00);
    usum->version(GameVersion::US);
    auto pk7 = pokemon<Generation::SEVEN>(Species::Pikachu);
    pk7->checksum(u16(pk7->checksum() + 1));
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pk7), *usum).check.refusal == ImportRefusal::Corrupted);
    CHECK(PokemonTransfer::prepareForSave(PokemonPayload{7, {1, 2, 3}}, *usum).check.refusal
          == ImportRefusal::Corrupted);
    CHECK(PokemonTransfer::prepareForSave(PokemonPayload{}, *usum).check.refusal == ImportRefusal::Corrupted);
}

void sameGenerationChecksTheTargetGame() {
    auto sm = blankSave<SavUSUM>(0x6CC00);
    sm->version(GameVersion::SN);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::SEVEN>(Species::Zeraora)), *sm).check.refusal
          == ImportRefusal::NotInGame);
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*pokemon<Generation::SEVEN>(Species::Pikachu)), *sm).check.allowed());

    auto bw = blankSave<SavBW>(0x80000);
    bw->version(GameVersion::B);
    auto gen4Giratina = pokemon<Generation::FOUR>(Species::Giratina, 1);
    gen4Giratina->heldItem(112); // Griseous Orb
    gen4Giratina->refreshChecksum();
    CHECK(PokemonTransfer::prepareForSave(payloadOf(*gen4Giratina), *bw).check.allowed());
}

// writeChecked calls trade() before the write and dex() after it: PKSM must accept both.
void tradeHooksUpdateHandlerAndDex() {
    auto usum = blankSave<SavUSUM>(0x6CC00);
    usum->version(GameVersion::US);
    usum->otName("ME");
    auto pk7 = pokemon<Generation::SEVEN>(Species::Pikachu);
    pk7->otName("OTHER");
    const int caught = usum->dexCaught();
    usum->trade(*pk7);
    usum->pkm(*pk7, 0, 0, false);
    usum->dex(*pk7);
    CHECK(pk7->currentHandler() == PKXHandler::NonOT);
    CHECK(usum->pkm(0, 0)->currentHandler() == PKXHandler::NonOT);
    CHECK(usum->dexCaught() == caught + 1);
}

}

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"canConvertFollowsTheOfficialChain", canConvertFollowsTheOfficialChain},
        {"usumOnlyValuesStayOutOfSunMoon", usumOnlyValuesStayOutOfSunMoon},
        {"orasOnlyValuesStayOutOfXY", orasOnlyValuesStayOutOfXY},
        {"b2w2OnlyFormsStayOutOfBW", b2w2OnlyFormsStayOutOfBW},
        {"platinumOnlyFormsStayOutOfDP", platinumOnlyFormsStayOutOfDP},
        {"gameBoyPokemonFitTheirGames", gameBoyPokemonFitTheirGames},
        {"conversionReportsDroppedItemAndHmMoves", conversionReportsDroppedItemAndHmMoves},
        {"gameBoyPokemonReachOnlyGen7", gameBoyPokemonReachOnlyGen7},
        {"cosplayPikachuStaysInGen6", cosplayPikachuStaysInGen6},
        {"eggsDoNotChangeGeneration", eggsDoNotChangeGeneration},
        {"badChecksumIsRefused", badChecksumIsRefused},
        {"sameGenerationChecksTheTargetGame", sameGenerationChecksTheTargetGame},
        {"tradeHooksUpdateHandlerAndDex", tradeHooksUpdateHandlerAndDex},
    };
    for (const auto& [name, body] : tests) {
        const int before = failures;
        try {
            body();
        } catch (const std::exception& exception) {
            ++failures;
            std::printf("FAIL  %s threw: %s\n", name, exception.what());
        }
        std::printf("%s %s\n", failures == before ? "ok   " : "FAIL ", name);
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
