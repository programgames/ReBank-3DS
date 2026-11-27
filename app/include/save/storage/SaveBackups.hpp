#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace SaveBackups {
inline constexpr std::size_t Kept = 10;

// Writes `data` to `directory`/<YYYYMMDD-HHMMSS>.sav and checks it, then removes the oldest
// backups past `Kept`. False when the backup is not on the SD card.
bool store(const std::string& directory, std::span<const std::uint8_t> data);
}
