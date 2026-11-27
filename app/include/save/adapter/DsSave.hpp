#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DsSave {
// Whether `code` is a DS game whose save `valid` knows how to check.
bool covers(std::string_view code);
// Whether `data` is a save of the game family of `code`, with PKSM's block checks.
// A blank card, a bad read or the save of another version is refused.
bool valid(std::string_view code, const std::uint8_t* data, std::size_t size);
// Appends the bytes of `original` past the end of `image` (a DeSmuME .dsv footer), so that a
// rewrite keeps them.
void keepFooter(std::vector<std::uint8_t>& image, const std::vector<std::uint8_t>& original);
}
