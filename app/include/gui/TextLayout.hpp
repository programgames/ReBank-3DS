#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// Line breaking by measured width. Cuts land on UTF-8 code-point boundaries only, so an accented
// letter is never split into two invalid halves. Needs no libctru: the caller supplies the measure.
namespace TextLayout {
using Measure = std::function<float(std::string_view)>;

// Byte length of the code point that starts at value[index]; 1 for a stray or truncated byte.
std::size_t codePointLength(std::string_view value, std::size_t index);

// Breaks at spaces and at '\n'; a word wider than the line is cut between code points. When more
// than maxLines lines result, the last kept line ends with an ellipsis.
std::vector<std::string> wrap(std::string_view text, float width, const Measure& measure,
                              std::size_t maxLines = std::numeric_limits<std::size_t>::max());

// The text itself when it fits, otherwise its longest prefix that fits with an ellipsis appended.
std::string fitWithEllipsis(std::string_view text, float width, const Measure& measure);
}
