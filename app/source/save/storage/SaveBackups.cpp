#include "save/storage/SaveBackups.hpp"

#include "platform/FileSystem.hpp"
#include "platform/FsGuard.hpp"
#include "platform/Logger.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <vector>

namespace SaveBackups {
namespace {
constexpr std::string_view Extension = ".sav";

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    char text[32] = "00000000-000000";
    if (local) {
        std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", local);
    }
    return text;
}

// The names sort by age, because they start with the timestamp.
std::vector<std::string> backupNames(const std::string& directory) {
    std::vector<std::string> names;
    DIR* listing = ::opendir(directory.c_str());
    if (!listing) {
        return names;
    }
    while (const dirent* entry = ::readdir(listing)) {
        const std::string_view name(entry->d_name);
        if (name.size() > Extension.size() && name.substr(name.size() - Extension.size()) == Extension) {
            names.emplace_back(name);
        }
    }
    ::closedir(listing);
    std::sort(names.begin(), names.end());
    return names;
}
}

bool store(const std::string& directory, std::span<const std::uint8_t> data) {
    const FsGuard guard;
    FileSystem::createDirectories(directory);
    const std::string name = timestamp() + std::string(Extension);
    const std::string path = directory + "/" + name;
    const std::optional<std::vector<std::uint8_t>> stored =
        FileSystem::write(path, data) ? FileSystem::read(path, data.size()) : std::nullopt;
    if (!stored || !std::equal(stored->begin(), stored->end(), data.begin(), data.end())) {
        std::remove(path.c_str());
        Logger::instance().error("Save backup could not be written to " + path);
        return false;
    }
    // The new backup always stays, even when a clock set back sorts it before the older ones.
    std::vector<std::string> names = backupNames(directory);
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    for (std::size_t index = 0; index + Kept < names.size() + 1; ++index) {
        std::remove((directory + "/" + names[index]).c_str());
    }
    Logger::instance().info("Save backed up to " + path);
    return true;
}
}
