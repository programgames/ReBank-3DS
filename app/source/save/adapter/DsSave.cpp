#include "save/adapter/DsSave.hpp"

#include <utils/crypto.hpp>

namespace DsSave {
namespace {
constexpr std::size_t SaveSize = 0x80000;
constexpr std::size_t DsvSize = 0x8007A;
constexpr std::size_t BackupBlockOffset = 0x40000;

enum class Family { DiamondPearl, Platinum, HeartGoldSoulSilver, BlackWhite, Black2White2, Unknown };

Family familyOf(std::string_view code) {
    if (code == "diamond" || code == "pearl") {
        return Family::DiamondPearl;
    }
    if (code == "platinum") {
        return Family::Platinum;
    }
    if (code == "heartgold" || code == "soulsilver") {
        return Family::HeartGoldSoulSilver;
    }
    if (code == "black" || code == "white") {
        return Family::BlackWhite;
    }
    if (code == "black2" || code == "white2") {
        return Family::Black2White2;
    }
    return Family::Unknown;
}

std::uint32_t readU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8
        | static_cast<std::uint32_t>(data[2]) << 16 | static_cast<std::uint32_t>(data[3]) << 24;
}

// Sav::validSequence: a Gen 4 general block ends with its size and the save format date.
bool validSequence(const std::uint8_t* data, std::size_t offset) {
    constexpr std::uint32_t DateInternational = 0x20060623;
    constexpr std::uint32_t DateKorean = 0x20070903;
    if (readU32(data + offset - 0xC) != (offset & 0xFFFF)) {
        return false;
    }
    const std::uint32_t date = readU32(data + offset - 0x8);
    return date == DateInternational || date == DateKorean;
}

bool validGeneration4(const std::uint8_t* data, std::size_t offset) {
    return validSequence(data, offset) || validSequence(data, offset + BackupBlockOffset);
}

// Sav::checkDSType: the Gen 5 save information block ends with its CCITT-16 checksum.
bool validGeneration5(const std::uint8_t* data, std::size_t blockEnd, std::size_t length) {
    const std::size_t start = blockEnd - 0x100;
    const std::size_t stored = data[start + length + 0xE] | data[start + length + 0xF] << 8;
    return stored == pksm::crypto::ccitt16({data + start, length});
}
}

bool covers(std::string_view code) {
    return familyOf(code) != Family::Unknown;
}

bool valid(std::string_view code, const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || (size != SaveSize && size != DsvSize)) {
        return false;
    }
    switch (familyOf(code)) {
        case Family::DiamondPearl:
            return validGeneration4(data, 0xC100);
        case Family::Platinum:
            return validGeneration4(data, 0xCF2C);
        case Family::HeartGoldSoulSilver:
            return validGeneration4(data, 0xF628);
        case Family::BlackWhite:
            return validGeneration5(data, 0x24000, 0x8C);
        case Family::Black2White2:
            return validGeneration5(data, 0x26000, 0x94);
        case Family::Unknown:
            break;
    }
    return false;
}

void keepFooter(std::vector<std::uint8_t>& image, const std::vector<std::uint8_t>& original) {
    if (original.size() > image.size()) {
        image.insert(image.end(), original.begin() + static_cast<std::ptrdiff_t>(image.size()), original.end());
    }
}
}
