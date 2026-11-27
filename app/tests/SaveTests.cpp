// Host-side tests for the save storage rules that run without libctru: DS save validation, the
// .dsv footer, the recovery of an interrupted SD replace and the backup rotation.

#include "platform/FileSystem.hpp"
#include "save/adapter/DsSave.hpp"
#include "save/storage/SaveBackups.hpp"
#include "save/storage/SaveFiles.hpp"

#include <utils/crypto.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

constexpr std::size_t DsSize = 0x80000;

void putU32(Bytes& data, std::size_t offset, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) {
        data[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
    }
}

// A Gen 4 general block footer, as the game writes it.
Bytes generation4Save(std::size_t blockEnd, std::uint8_t fill = 0) {
    Bytes data(DsSize, fill);
    putU32(data, blockEnd - 0xC, blockEnd & 0xFFFF);
    putU32(data, blockEnd - 0x8, 0x20060623);
    return data;
}

// A Gen 5 save information block with its checksum.
Bytes generation5Save(std::size_t blockEnd, std::size_t length) {
    Bytes data(DsSize, 0x5A);
    const std::size_t start = blockEnd - 0x100;
    const std::uint16_t checksum = pksm::crypto::ccitt16({data.data() + start, length});
    data[start + length + 0xE] = static_cast<std::uint8_t>(checksum);
    data[start + length + 0xF] = static_cast<std::uint8_t>(checksum >> 8);
    return data;
}

bool valid(const char* code, const Bytes& data) {
    return DsSave::valid(code, data.data(), data.size());
}

void dsSavesMatchTheirFamily() {
    const Bytes diamond = generation4Save(0xC100);
    const Bytes platinum = generation4Save(0xCF2C);
    const Bytes heartGold = generation4Save(0xF628);
    const Bytes black = generation5Save(0x24000, 0x8C);
    const Bytes black2 = generation5Save(0x26000, 0x94);

    CHECK(valid("diamond", diamond) && valid("pearl", diamond));
    CHECK(valid("platinum", platinum));
    CHECK(valid("heartgold", heartGold) && valid("soulsilver", heartGold));
    CHECK(valid("black", black) && valid("white", black));
    CHECK(valid("black2", black2) && valid("white2", black2));

    CHECK(!valid("diamond", platinum));
    CHECK(!valid("platinum", diamond));
    CHECK(!valid("heartgold", platinum));
    CHECK(!valid("black", black2));
    CHECK(!valid("black2", black));
}

void dsSaveUsesTheBackupBlock() {
    Bytes data(DsSize, 0);
    putU32(data, 0xC100 + 0x40000 - 0xC, 0xC100);
    putU32(data, 0xC100 + 0x40000 - 0x8, 0x20070903);
    CHECK(valid("diamond", data));
}

void blankOrCutDsSavesAreRefused() {
    for (const std::uint8_t fill : {0x00, 0xFF}) {
        const Bytes blank(DsSize, fill);
        for (const char* code : {"diamond", "platinum", "heartgold", "black", "black2"}) {
            CHECK(!valid(code, blank));
        }
    }
    Bytes cut = generation4Save(0xC100);
    cut.resize(DsSize - 1);
    CHECK(!valid("diamond", cut));
    CHECK(!DsSave::valid("diamond", nullptr, DsSize));
}

void dsvSaveIsValidAndKeepsItsFooter() {
    Bytes dsv = generation4Save(0xC100);
    for (std::size_t index = 0; index < 0x7A; ++index) {
        dsv.push_back(static_cast<std::uint8_t>(index + 1));
    }
    CHECK(valid("diamond", dsv));

    Bytes rewritten(dsv.begin(), dsv.begin() + DsSize);
    rewritten[0] = 0x42;
    DsSave::keepFooter(rewritten, dsv);
    CHECK(rewritten.size() == dsv.size());
    CHECK(std::equal(rewritten.begin() + DsSize, rewritten.end(), dsv.begin() + DsSize));
    CHECK(rewritten[0] == 0x42);

    Bytes plain(DsSize, 1);
    DsSave::keepFooter(plain, Bytes(DsSize, 2));
    CHECK(plain == Bytes(DsSize, 1));
}

void putFile(const fs::path& path, const std::string& text) {
    CHECK(FileSystem::write(path.string(), text));
}

std::string readFile(const fs::path& path) {
    const auto data = FileSystem::read(path.string(), 1 << 20);
    return data ? std::string(data->begin(), data->end()) : std::string{};
}

void interruptedReplaceIsRecovered(const fs::path& root) {
    const fs::path save = root / "game.sav";
    putFile(root / "game.sav.tmp", "new");
    putFile(root / "game.sav.prev", "old");
    CHECK(SaveFiles::existsAfterRecovery(save.string()));
    CHECK(readFile(save) == "new");

    const fs::path older = root / "older.sav";
    putFile(root / "older.sav.prev", "old");
    CHECK(SaveFiles::existsAfterRecovery(older.string()));
    CHECK(readFile(older) == "old");

    CHECK(!SaveFiles::existsAfterRecovery((root / "none.sav").string()));

    putFile(root / "kept.sav", "current");
    putFile(root / "kept.sav.tmp", "stray");
    CHECK(SaveFiles::existsAfterRecovery((root / "kept.sav").string()));
    CHECK(readFile(root / "kept.sav") == "current");
}

void backupsKeepTheNewest(const fs::path& root) {
    const fs::path directory = root / "backups" / "diamond";
    fs::create_directories(directory);
    for (int index = 0; index < 12; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "20200101-0000%02d.sav", index);
        putFile(directory / name, "old");
    }
    putFile(directory / "notes.txt", "kept");

    const Bytes save(DsSize, 7);
    CHECK(SaveBackups::store(directory.string(), save));

    std::vector<std::string> backups;
    Bytes newest;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".sav") {
            backups.push_back(name);
        }
        if (name.rfind("2020", 0) != 0 && entry.path().extension() == ".sav") {
            newest = *FileSystem::read(entry.path().string(), DsSize);
        }
    }
    CHECK(backups.size() == SaveBackups::Kept);
    CHECK(newest == save);
    CHECK(!fs::exists(directory / "20200101-000000.sav"));
    CHECK(!fs::exists(directory / "20200101-000002.sav"));
    CHECK(fs::exists(directory / "20200101-000003.sav"));
    CHECK(fs::exists(directory / "notes.txt"));
}

// Backups named by a clock that has since been set back still lose to the new one.
void newBackupSurvivesAClockSetBack(const fs::path& root) {
    const fs::path directory = root / "backups" / "platinum";
    fs::create_directories(directory);
    for (int index = 0; index < 12; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "29991231-0000%02d.sav", index);
        putFile(directory / name, "future");
    }
    const Bytes save(DsSize, 9);
    CHECK(SaveBackups::store(directory.string(), save));

    std::size_t count = 0;
    bool newKept = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        ++count;
        newKept = newKept || entry.path().filename().string().rfind("2999", 0) != 0;
    }
    CHECK(count == SaveBackups::Kept);
    CHECK(newKept);
}
}

int main() {
    const fs::path root = fs::path("build") / "save-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    dsSavesMatchTheirFamily();
    dsSaveUsesTheBackupBlock();
    blankOrCutDsSavesAreRefused();
    dsvSaveIsValidAndKeepsItsFooter();
    interruptedReplaceIsRecovered(root);
    backupsKeepTheNewest(root);
    newBackupSurvivesAClockSetBack(root);

    fs::remove_all(root);
    std::printf(failures == 0 ? "save tests: all passed\n" : "save tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
