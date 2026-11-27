#include "network/SessionTokens.hpp"

#include <algorithm>
#include <utility>

namespace {
constexpr int Unauthorized = 401;
}

void SessionTokens::set(const AccountSession& session) {
    std::lock_guard guard(lock_);
    session_ = session;
    expired_.clear();
    renewed_.reset();
}

void SessionTokens::clear() {
    set({});
}

bool SessionTokens::knownLocked(const std::string& token) const {
    return !token.empty()
        && (token == session_.accessToken || std::find(expired_.begin(), expired_.end(), token) != expired_.end());
}

std::string SessionTokens::current(const std::string& used) const {
    std::lock_guard guard(lock_);
    return knownLocked(used) ? session_.accessToken : used;
}

SessionTokens::RenewResult SessionTokens::renew(const std::string& used, const Refresher& refresher) {
    std::lock_guard serial(renewing_);
    std::string refreshToken;
    {
        std::lock_guard guard(lock_);
        if (!knownLocked(used)) {
            return {};
        }
        if (used != session_.accessToken) {
            return {Renewal::Renewed, session_.accessToken};
        }
        refreshToken = session_.refreshToken;
    }
    const AuthResult result = refresher(refreshToken);
    std::lock_guard guard(lock_);
    if (session_.refreshToken != refreshToken) {
        return {};
    }
    if (!result.success) {
        const bool rejected = !result.networkError && result.httpStatus == Unauthorized;
        return {rejected ? Renewal::Rejected : Renewal::Failed, {}};
    }
    expired_.push_back(session_.accessToken);
    session_ = result.session;
    renewed_ = session_;
    return {Renewal::Renewed, session_.accessToken};
}

std::optional<AccountSession> SessionTokens::takeRenewed() {
    std::lock_guard guard(lock_);
    return std::exchange(renewed_, std::nullopt);
}
