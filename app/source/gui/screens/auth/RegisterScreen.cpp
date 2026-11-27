#include "gui/screens/auth/RegisterScreen.hpp"

#include "account/InputValidation.hpp"

#include <3ds.h>

namespace {
enum RegisterField : std::size_t { Username, Email, Password };
}

RegisterScreen::RegisterScreen(AppContext& app)
    : AuthFormScreen(app, ScreenId::Register, TextId::CreateAccount,
                     {{TextId::Username, {24.0F, 48.0F, 272.0F, 34.0F}, false, 1.0F, {}},
                      {TextId::Email, {24.0F, 88.0F, 272.0F, 34.0F}, false, -1.0F, {}},
                      {TextId::Password, {24.0F, 128.0F, 272.0F, 34.0F}, true, 1.0F, {}}},
                     std::nullopt, UiRect{24.0F, 166.0F, 272.0F, 26.0F}) {}

void RegisterScreen::update(const InputFrame& input) {
    Policy policy;
    if (policyTask_.poll(policy)) {
        app_.status.clear();
        if (policy) {
            policy->insert(policy->begin(), PolicyBlock{PolicyBlock::Kind::Subheading, std::string(text(TextId::PolicyAgeConfirmation)), {}});
            reader_.open(std::move(*policy));
        } else {
            app_.navigator.showError(TextId::PrivacyUnavailableTitle, TextId::PrivacyUnavailableMessage);
        }
        return;
    }
    if (reader_.isOpen()) {
        const PolicyReader::Outcome outcome = reader_.update(input);
        if (outcome != PolicyReader::Outcome::None) {
            setConsentAccepted(outcome == PolicyReader::Outcome::Accepted);
        }
        return;
    }
    if (policyTask_.running()) {
        return;
    }
    AuthFormScreen::update(input);
}

void RegisterScreen::render() {
    if (reader_.isOpen()) {
        reader_.render(ui(), text(TextId::PrivacyRequiredTitle), text(TextId::PolicyDecline), text(TextId::PolicyAccept));
        return;
    }
    AuthFormScreen::render();
}

void RegisterScreen::openConsentDocument() {
    if (policyTask_.running() || reader_.isOpen()) {
        return;
    }
    const std::string_view code = app_.text.languageCode();
    app_.status = text(TextId::PrivacyLoading);
    // The server may have no policy in the console language: English is the fallback.
    if (!policyTask_.start([this, code]() {
            Policy policy = app_.accountApi.privacyPolicy(code);
            return policy || code == "en" ? policy : app_.accountApi.privacyPolicy("en");
        })) {
        app_.status.clear();
        app_.navigator.showError(TextId::PrivacyUnavailableTitle, TextId::PrivacyUnavailableMessage);
    }
}

void RegisterScreen::submit() {
    if (rejectIf(!isValidNewUsername(value(Username)), TextId::InvalidUsernameTitle, TextId::InvalidUsernameMessage)
        || rejectIf(!isValidEmail(value(Email)), TextId::InvalidEmailTitle, TextId::InvalidEmailMessage)
        || rejectIf(!isValidPassword(value(Password)), TextId::InvalidPasswordTitle, TextId::InvalidPasswordMessage)
        || rejectIf(!consentAccepted(), TextId::PrivacyRequiredTitle, TextId::PrivacyRequiredMessage)) {
        return;
    }
    app_.navigator.beginAuth(AuthRequest::registration(value(Username), value(Email), value(Password)));
}
