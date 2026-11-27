#include "gui/TextLayout.hpp"

namespace TextLayout {
namespace {
constexpr std::string_view Ellipsis = "...";

bool continuationByte(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Longest prefix of word that fits in width, never shorter than its first code point.
std::size_t fittingPrefix(std::string_view word, float width, const Measure& measure) {
    const std::size_t minimum = codePointLength(word, 0);
    std::size_t length = word.size();
    while (length > minimum && measure(word.substr(0, length)) > width) {
        --length;
        while (length > minimum && continuationByte(word[length])) {
            --length;
        }
    }
    return length;
}

std::string withEllipsis(std::string_view text, float width, const Measure& measure) {
    std::size_t length = text.size();
    while (length > 0 && measure(std::string(text.substr(0, length)) + std::string(Ellipsis)) > width) {
        --length;
        while (length > 0 && continuationByte(text[length])) {
            --length;
        }
    }
    while (length > 0 && text[length - 1] == ' ') {
        --length;
    }
    return std::string(text.substr(0, length)) + std::string(Ellipsis);
}

void wrapParagraph(std::string_view paragraph, float width, const Measure& measure, std::vector<std::string>& lines) {
    std::string line;
    while (!paragraph.empty()) {
        const std::size_t space = paragraph.find(' ');
        std::string_view word = paragraph.substr(0, space);
        paragraph = space == std::string_view::npos ? std::string_view{} : paragraph.substr(space + 1);
        if (word.empty()) {
            continue;
        }
        const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (measure(candidate) <= width) {
            line = candidate;
            continue;
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        while (!word.empty() && measure(word) > width) {
            const std::size_t cut = fittingPrefix(word, width, measure);
            lines.emplace_back(word.substr(0, cut));
            word.remove_prefix(cut);
        }
        line = std::string(word);
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
}
}

std::size_t codePointLength(std::string_view value, std::size_t index) {
    if (index >= value.size()) {
        return 0;
    }
    const auto lead = static_cast<unsigned char>(value[index]);
    const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (index + length > value.size()) {
        return 1;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        if (!continuationByte(value[index + offset])) {
            return 1;
        }
    }
    return length;
}

std::vector<std::string> wrap(std::string_view text, float width, const Measure& measure, std::size_t maxLines) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        wrapParagraph(text.substr(start, end - start), width, measure, lines);
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    if (lines.size() > maxLines) {
        lines.resize(maxLines);
        if (!lines.empty()) {
            lines.back() = withEllipsis(lines.back(), width, measure);
        }
    }
    return lines;
}

std::string fitWithEllipsis(std::string_view text, float width, const Measure& measure) {
    return measure(text) <= width ? std::string(text) : withEllipsis(text, width, measure);
}
}
