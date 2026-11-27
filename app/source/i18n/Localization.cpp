#include "i18n/Localization.hpp"

#include <3ds.h>

Localization::Localization() : translation_(&Translations::english()), systemLanguage_(CFG_LANGUAGE_EN) {
    u8 language = CFG_LANGUAGE_EN;
    if (R_FAILED(CFGU_GetSystemLanguage(&language))) {
        return;
    }
    systemLanguage_ = language;
    switch (language) {
        case CFG_LANGUAGE_DE:
            translation_ = &Translations::german();
            break;
        case CFG_LANGUAGE_FR:
            translation_ = &Translations::french();
            break;
        case CFG_LANGUAGE_ES:
            translation_ = &Translations::spanish();
            break;
        case CFG_LANGUAGE_IT:
            translation_ = &Translations::italian();
            break;
        case CFG_LANGUAGE_PT:
            translation_ = &Translations::portuguese();
            break;
        default:
            break;
    }
}

pksm::Language Localization::dataLanguage() const {
    // romfs/assets/strings has no Dutch, Portuguese or Russian tables: those consoles get English.
    switch (systemLanguage_) {
        case CFG_LANGUAGE_JP:
            return pksm::Language::JPN;
        case CFG_LANGUAGE_FR:
            return pksm::Language::FRE;
        case CFG_LANGUAGE_DE:
            return pksm::Language::GER;
        case CFG_LANGUAGE_IT:
            return pksm::Language::ITA;
        case CFG_LANGUAGE_ES:
            return pksm::Language::SPA;
        case CFG_LANGUAGE_ZH:
            return pksm::Language::CHS;
        case CFG_LANGUAGE_KO:
            return pksm::Language::KOR;
        case CFG_LANGUAGE_TW:
            return pksm::Language::CHT;
        default:
            return pksm::Language::ENG;
    }
}

std::string_view Localization::languageCode() const {
    switch (systemLanguage_) {
        case CFG_LANGUAGE_JP:
            return "ja";
        case CFG_LANGUAGE_FR:
            return "fr";
        case CFG_LANGUAGE_DE:
            return "de";
        case CFG_LANGUAGE_IT:
            return "it";
        case CFG_LANGUAGE_ES:
            return "es";
        case CFG_LANGUAGE_ZH:
        case CFG_LANGUAGE_TW:
            return "zh";
        case CFG_LANGUAGE_KO:
            return "ko";
        case CFG_LANGUAGE_NL:
            return "nl";
        case CFG_LANGUAGE_PT:
            return "pt";
        case CFG_LANGUAGE_RU:
            return "ru";
        default:
            return "en";
    }
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
