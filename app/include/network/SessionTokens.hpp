#pragma once

#include "network/ApiTypes.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The signed-in session, shared by the UI thread and the network workers. A worker copies the access
// token when its job starts; after a refresh, current() maps that stale copy to the new token. Tokens of
// an earlier session (before a sign-out) are never mapped, so a late job cannot act on another account.
class SessionTokens {
public:
    enum class Renewal { Renewed, Rejected, Failed };
    struct RenewResult {
        Renewal outcome = Renewal::Failed;
        std::string accessToken;
    };
    using Refresher = std::function<AuthResult(const std::string& refreshToken)>;

    void set(const AccountSession& session);
    void clear();
    std::string current(const std::string& used) const;

    // Called after a 401 for `used`. Refreshes once per expired token, even when several workers fail
    // at the same time: the others get the token the first one obtained.
    RenewResult renew(const std::string& used, const Refresher& refresher);

    // The session a refresh produced, for the UI thread to adopt and store. Empty when nothing changed.
    std::optional<AccountSession> takeRenewed();

private:
    bool knownLocked(const std::string& token) const;

    mutable std::mutex lock_;
    std::mutex renewing_;
    AccountSession session_;
    std::vector<std::string> expired_;
    std::optional<AccountSession> renewed_;
};
