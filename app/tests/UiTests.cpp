// Host-side tests for the UI text rules: line breaking on code points and the translation tables.

#include "gui/TextLayout.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// One width unit per code point, so the tests read in characters.
float codePoints(std::string_view text) {
    float count = 0.0F;
    for (std::size_t index = 0; index < text.size(); index += TextLayout::codePointLength(text, index)) {
        count += 1.0F;
    }
    return count;
}

bool validUtf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const std::size_t length = TextLayout::codePointLength(text, index);
        if (length == 1 && static_cast<unsigned char>(text[index]) >= 0x80) {
            return false;
        }
        index += length;
    }
    return true;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += "[" + line + "]";
    }
    return text;
}

void wrapBreaksAtSpacesAndNewlines() {
    const auto lines = TextLayout::wrap("Vérifie ta connexion\net réessaie.", 12.0F, codePoints);
    check(joined(lines) == "[Vérifie ta][connexion][et réessaie.]", "wrap at spaces and newlines: " + joined(lines));
}

void wrapCutsLongWordsBetweenCodePoints() {
    // "é" is two bytes; a byte-based cut at 3 would split it.
    const auto lines = TextLayout::wrap("aaéééé", 3.0F, codePoints);
    check(joined(lines) == "[aaé][ééé]", "long word cut on code points: " + joined(lines));
    for (const std::string& line : lines) {
        check(validUtf8(line), "long word line is valid UTF-8: " + line);
    }
}

void wrapKeepsAWideFirstCodePoint() {
    // A single glyph wider than the line must still be emitted whole, without looping.
    const auto lines = TextLayout::wrap("中文", 0.5F, codePoints);
    check(joined(lines) == "[中][文]", "glyph wider than the line: " + joined(lines));
}

void wrapEllipsizesDroppedLines() {
    const auto lines = TextLayout::wrap("un deux trois quatre", 7.0F, codePoints, 2);
    check(joined(lines) == "[un deux][troi...]", "max lines with ellipsis: " + joined(lines));
    check(TextLayout::wrap("un deux", 7.0F, codePoints, 1).size() == 1, "max lines exact fit");
}

void fitKeepsOrCutsWithEllipsis() {
    check(TextLayout::fitWithEllipsis("Niveau", 6.0F, codePoints) == "Niveau", "fit keeps short text");
    const std::string cut = TextLayout::fitWithEllipsis("Fähigkeit", 6.0F, codePoints);
    check(cut == "Fäh...", "fit cuts with ellipsis: " + cut);
    check(TextLayout::fitWithEllipsis("ab cdef", 6.0F, codePoints) == "ab...", "fit trims the space before ...");
    check(validUtf8(TextLayout::fitWithEllipsis("éééééééé", 5.0F, codePoints)), "fit result is valid UTF-8");
}
}

int main() {
    wrapBreaksAtSpacesAndNewlines();
    wrapCutsLongWordsBetweenCodePoints();
    wrapKeepsAWideFirstCodePoint();
    wrapEllipsizesDroppedLines();
    fitKeepsOrCutsWithEllipsis();
    std::printf(failures == 0 ? "ui_tests ok\n" : "ui_tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
