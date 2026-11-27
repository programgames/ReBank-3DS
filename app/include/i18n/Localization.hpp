#pragma once

#include "i18n/Translation.hpp"

#include <enums/Language.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

class Localization {
public:
    Localization();
    std::string_view get(TextId id) const;
    std::string format(TextId id, std::initializer_list<std::string_view> arguments) const;

    // Language of the species, move, item, ability and nature names (PKSM tables in romfs).
    pksm::Language dataLanguage() const { return dataLanguage_; }
    // ISO 639-1 code of the console language, for server documents such as the privacy policy.
    std::string_view languageCode() const { return languageCode_; }

private:
    const Translation* translation_;
    pksm::Language dataLanguage_;
    std::string_view languageCode_;
};
