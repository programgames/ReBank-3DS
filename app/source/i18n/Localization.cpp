#include "i18n/Localization.hpp"

#include <3ds.h>

namespace {
struct ConsoleLanguage {
    CFG_Language console;
    const Translation& (*table)();
    pksm::Language data; // romfs/assets/strings has no Dutch, Portuguese or Russian names: English
    std::string_view code; // ISO 639-1, for server documents such as the privacy policy
};

constexpr ConsoleLanguage English{CFG_LANGUAGE_EN, Translations::english, pksm::Language::ENG, "en"};

constexpr ConsoleLanguage Languages[] = {
    {CFG_LANGUAGE_JP, Translations::english, pksm::Language::JPN, "ja"},
    English,
    {CFG_LANGUAGE_FR, Translations::french, pksm::Language::FRE, "fr"},
    {CFG_LANGUAGE_DE, Translations::german, pksm::Language::GER, "de"},
    {CFG_LANGUAGE_IT, Translations::italian, pksm::Language::ITA, "it"},
    {CFG_LANGUAGE_ES, Translations::spanish, pksm::Language::SPA, "es"},
    {CFG_LANGUAGE_ZH, Translations::english, pksm::Language::CHS, "zh-CN"},
    {CFG_LANGUAGE_KO, Translations::english, pksm::Language::KOR, "ko"},
    {CFG_LANGUAGE_NL, Translations::english, pksm::Language::ENG, "nl"},
    {CFG_LANGUAGE_PT, Translations::portuguese, pksm::Language::ENG, "pt"},
    {CFG_LANGUAGE_RU, Translations::english, pksm::Language::ENG, "ru"},
    {CFG_LANGUAGE_TW, Translations::english, pksm::Language::CHT, "zh-TW"},
};

const ConsoleLanguage& consoleLanguage() {
    u8 language = CFG_LANGUAGE_EN;
    if (R_SUCCEEDED(CFGU_GetSystemLanguage(&language))) {
        for (const ConsoleLanguage& entry : Languages) {
            if (entry.console == language) {
                return entry;
            }
        }
    }
    return English;
}
}

Localization::Localization() {
    const ConsoleLanguage& language = consoleLanguage();
    translation_ = &language.table();
    dataLanguage_ = language.data;
    languageCode_ = language.code;
}

std::string_view Localization::get(TextId id) const {
    const std::string_view text = (*translation_)[at(id)];
    return text.empty() ? Translations::english()[at(id)] : text;
}

std::string Localization::format(TextId id, std::initializer_list<std::string_view> arguments) const {
    const std::string_view pattern = get(id);
    std::string text;
    text.reserve(pattern.size() + 16);
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const bool placeholder = pattern[index] == '{' && index + 2 < pattern.size()
            && pattern[index + 1] >= '0' && pattern[index + 1] <= '9' && pattern[index + 2] == '}';
        if (!placeholder) {
            text.push_back(pattern[index]);
            continue;
        }
        const auto argument = static_cast<std::size_t>(pattern[index + 1] - '0');
        if (argument < arguments.size()) {
            text.append(*(arguments.begin() + argument));
        }
        index += 2;
    }
    return text;
}
