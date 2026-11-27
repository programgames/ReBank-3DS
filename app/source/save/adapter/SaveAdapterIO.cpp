#include "platform/Logger.hpp"
#include "save/adapter/SaveAdapter.hpp"
#include "save/pokemon/PokemonTransfer.hpp"

#include <pkx/PKX.hpp>
#include <sav/Sav.hpp>

#include <algorithm>

namespace {
std::string eggMetInfo(const pksm::PKX& pkm) {
    return "egg=" + std::string(pkm.egg() ? "1" : "0")
        + " metLocation=" + std::to_string(pkm.metLocation())
        + " metDate=" + std::to_string(pkm.metDate().year()) + "-"
        + std::to_string(pkm.metDate().month()) + "-"
        + std::to_string(pkm.metDate().day());
}

PokemonSummary summarize(const pksm::PKX& pkm, const std::string& gameCode, std::uint8_t format) {
    return PokemonSummary{
        static_cast<std::uint16_t>(pkm.species()),
        pkm.alternativeForm(),
        pkm.level(),
        pkm.shiny(),
        pkm.generation() == pksm::Generation::ONE ? std::uint16_t{0} : pkm.heldItem(),
        pkm.nickname(),
        pkm.otName(),
        gameCode,
        format,
        pkm.type1(),
        pkm.type2(),
        pkm.version(),
        pkm.language(),
        {pkm.move(0), pkm.move(1), pkm.move(2), pkm.move(3)},
        pkm.ability(),
        pkm.nature(),
        pkm.gender()
    };
}

PokemonPayload payloadOf(const pksm::PKX& pkm, std::uint8_t format) {
    const auto raw = pkm.rawData();
    return {format, std::vector<std::uint8_t>(raw.begin(), raw.end())};
}

template <std::size_t Count, typename Reader>
SlotGroup<Count> readSlots(std::size_t count, const std::string& gameCode, std::uint8_t format,
                           const char* label, Reader&& read) {
    SlotGroup<Count> result;
    try {
        for (std::size_t slot = 0; slot < std::min(count, Count); ++slot) {
            auto parsed = read(slot);
            if (!parsed || static_cast<std::uint16_t>(parsed->species()) == 0) {
                continue;
            }
            result.summaries[slot] = summarize(*parsed, gameCode, format);
            result.payloads[slot] = payloadOf(*parsed, format);
        }
    } catch (const std::exception& exception) {
        Logger::instance().error(std::string(label) + " parse exception: " + exception.what());
    }
    return result;
}

const char* refusalName(PokemonTransfer::ImportRefusal refusal) {
    switch (refusal) {
        case PokemonTransfer::ImportRefusal::None: return "none";
        case PokemonTransfer::ImportRefusal::Generation: return "generation";
        case PokemonTransfer::ImportRefusal::Egg: return "egg changing generation";
        case PokemonTransfer::ImportRefusal::Corrupted: return "corrupted payload";
        case PokemonTransfer::ImportRefusal::NotInGame: return "not in this game";
    }
    return "unknown";
}

bool verifyWritten(const pksm::PKX* written, const pksm::PKX& expected, const std::string& location) {
    if (!written || written->generation() != expected.generation() || written->species() != expected.species()
        || !PokemonTransfer::checksumValid(*written)) {
        Logger::instance().error("write " + location + ": readback failed");
        return false;
    }
    if (written->egg() && !expected.egg()) {
        Logger::instance().error("write " + location + ": write turned a non-egg into an egg, refusing");
        return false;
    }
    return true;
}

// With asTrade, writes the Pokemon as a trade would bring it: handler and egg met data first, Pokedex after.
template <typename Store, typename Read>
bool writeChecked(pksm::Sav& save, const std::string& location, const PokemonPayload& payload, bool asTrade,
                  Store&& store, Read&& read) {
    PokemonTransfer::PreparedPokemon prepared = PokemonTransfer::prepareForSave(payload, save);
    if (!prepared.pokemon) {
        Logger::instance().warning("write " + location + ": refused, " + refusalName(prepared.check.refusal)
                                   + " (format " + std::to_string(payload.format) + ")");
        return false;
    }
    pksm::PKX& pkx = *prepared.pokemon;
    if (asTrade) {
        save.trade(pkx);
    }
    store(pkx);
    const auto written = read();
    if (!verifyWritten(written.get(), pkx, location)) {
        return false;
    }
    if (asTrade && !written->egg()) { // Sav2::dex does not skip eggs itself
        save.dex(*written);
    }
    Logger::instance().info("write " + location + ": species "
                            + std::to_string(static_cast<std::uint16_t>(written->species()))
                            + " " + eggMetInfo(*written) + " bytes=" + std::to_string(payload.data.size()));
    return true;
}
}

std::uint8_t SaveAdapter::saveFormat() const {
    return PokemonTransfer::pokemonFormat(save_->generation());
}

BoxSlots SaveAdapter::readBox(std::size_t box) const {
    if (!validBox(box)) {
        return {};
    }
    return readSlots<BoxSlotCount>(boxCapacity(), gameCode_, saveFormat(), "Box", [&](std::size_t slot) {
        return save_->pkm(static_cast<std::uint8_t>(box), static_cast<std::uint8_t>(slot));
    });
}

bool SaveAdapter::slotOccupied(std::size_t box, std::size_t slot) const {
    if (!validSlot(box, slot)) {
        return true;
    }
    try {
        auto parsed = save_->pkm(static_cast<std::uint8_t>(box), static_cast<std::uint8_t>(slot));
        return parsed && static_cast<std::uint16_t>(parsed->species()) != 0;
    } catch (const std::exception& exception) {
        Logger::instance().error("slotOccupied exception: " + std::string(exception.what()));
        return true;
    }
}

PartySlots SaveAdapter::readParty() const {
    if (!save_) {
        return {};
    }
    return readSlots<PartySlotCount>(partyCount(), gameCode_, saveFormat(), "Party", [&](std::size_t slot) {
        return save_->pkm(static_cast<std::uint8_t>(slot));
    });
}

std::size_t SaveAdapter::partyCount() const {
    return save_ ? std::min<std::size_t>(save_->partyCount(), PartySlotCount) : 0;
}

PokemonTransfer::ImportCheck SaveAdapter::checkImport(const PokemonPayload& payload) const {
    if (!save_) {
        return {PokemonTransfer::ImportRefusal::Corrupted, {}};
    }
    try {
        return PokemonTransfer::prepareForSave(payload, *save_).check;
    } catch (const std::exception& exception) {
        Logger::instance().error("checkImport exception: " + std::string(exception.what()));
        return {PokemonTransfer::ImportRefusal::Corrupted, {}};
    }
}

template <typename Edit>
bool SaveAdapter::edit(const char* action, Edit&& change) {
    try {
        const bool ok = change();
        dirty_ = dirty_ || ok;
        return ok;
    } catch (const std::exception& exception) {
        Logger::instance().error(std::string(action) + " exception: " + exception.what());
        return false;
    }
}

bool SaveAdapter::storeEmpty(const std::function<void(const pksm::PKX&)>& store) {
    const auto empty = save_->emptyPkm();
    if (empty) {
        store(*empty);
    }
    return empty != nullptr;
}

bool SaveAdapter::clearSlot(std::size_t box, std::size_t slot) {
    const auto boxIndex = static_cast<std::uint8_t>(box);
    const auto slotIndex = static_cast<std::uint8_t>(slot);
    return validSlot(box, slot) && edit("clearSlot", [&] {
        return storeEmpty([&](const pksm::PKX& empty) { save_->pkm(empty, boxIndex, slotIndex, false); });
    });
}

bool SaveAdapter::writePokemon(std::size_t box, std::size_t slot, const PokemonPayload& payload, bool asTrade) {
    const auto boxIndex = static_cast<std::uint8_t>(box);
    const auto slotIndex = static_cast<std::uint8_t>(slot);
    return validSlot(box, slot) && payload.known() && edit("writePokemon", [&] {
        return writeChecked(*save_,
            "box " + std::to_string(box + 1) + " slot " + std::to_string(slot + 1), payload, asTrade,
            [&](const pksm::PKX& pkm) { save_->pkm(pkm, boxIndex, slotIndex, false); },
            [&] { return save_->pkm(boxIndex, slotIndex); });
    });
}

bool SaveAdapter::clearPartySlot(std::size_t slot) {
    const auto slotIndex = static_cast<std::uint8_t>(slot);
    return validPartySlot(slot) && edit("clearPartySlot", [&] {
        beginPartyEdit();
        return storeEmpty([&](const pksm::PKX& empty) { save_->pkm(empty, slotIndex); });
    });
}

bool SaveAdapter::writePartyPokemon(std::size_t slot, const PokemonPayload& payload, bool asTrade) {
    const auto slotIndex = static_cast<std::uint8_t>(slot);
    return validPartySlot(slot) && payload.known() && edit("writePartyPokemon", [&] {
        beginPartyEdit();
        return writeChecked(*save_, "party slot " + std::to_string(slot + 1), payload, asTrade,
            [&](const pksm::PKX& pkm) { save_->pkm(pkm, slotIndex); },
            [&] { return save_->pkm(slotIndex); });
    });
}
