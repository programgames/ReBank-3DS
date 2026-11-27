// Host-side tests for SessionTokens: the access token renewal shared by the UI thread and the workers.

#include "network/SessionTokens.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ++failures;                                                      \
            std::printf("FAIL  %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                    \
    } while (false)

using Renewal = SessionTokens::Renewal;

AccountSession session(const char* access, const char* refresh) {
    return {"account", access, refresh, 50};
}

AuthResult granted(const char* access, const char* refresh) {
    return {true, {}, session(access, refresh), false, 200};
}

void renewMapsStaleCopies() {
    SessionTokens tokens;
    tokens.set(session("A1", "R1"));
    int calls = 0;
    std::string usedRefresh;
    const auto refresher = [&](const std::string& refreshToken) {
        ++calls;
        usedRefresh = refreshToken;
        return granted("A2", "R2");
    };

    const SessionTokens::RenewResult first = tokens.renew("A1", refresher);
    CHECK(first.outcome == Renewal::Renewed && first.accessToken == "A2");
    CHECK(calls == 1 && usedRefresh == "R1");
    CHECK(tokens.current("A1") == "A2");
    CHECK(tokens.current("A2") == "A2");
    CHECK(tokens.current("other") == "other");
    CHECK(tokens.current("") == "");

    // A worker that still holds A1 gets the new token without a second refresh.
    const SessionTokens::RenewResult late = tokens.renew("A1", refresher);
    CHECK(late.outcome == Renewal::Renewed && late.accessToken == "A2" && calls == 1);

    const std::optional<AccountSession> renewed = tokens.takeRenewed();
    CHECK(renewed && renewed->accessToken == "A2" && renewed->refreshToken == "R2");
    CHECK(!tokens.takeRenewed());

    // A second expiry refreshes with the rotated token, and both older copies map to the newest.
    tokens.renew("A2", [&](const std::string& refreshToken) {
        usedRefresh = refreshToken;
        return granted("A3", "R3");
    });
    CHECK(usedRefresh == "R2");
    CHECK(tokens.current("A1") == "A3" && tokens.current("A2") == "A3");
}

void concurrentExpiryRefreshesOnce() {
    SessionTokens tokens;
    tokens.set(session("A1", "R1"));
    std::atomic<int> calls{0};
    const auto refresher = [&](const std::string&) {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return granted("A2", "R2");
    };
    SessionTokens::RenewResult left;
    SessionTokens::RenewResult right;
    std::thread first([&] { left = tokens.renew("A1", refresher); });
    std::thread second([&] { right = tokens.renew("A1", refresher); });
    first.join();
    second.join();
    CHECK(calls == 1);
    CHECK(left.outcome == Renewal::Renewed && left.accessToken == "A2");
    CHECK(right.outcome == Renewal::Renewed && right.accessToken == "A2");
}

void refusalAndNetworkErrors() {
    SessionTokens tokens;
    tokens.set(session("A1", "R1"));
    const SessionTokens::RenewResult offline =
        tokens.renew("A1", [](const std::string&) { return AuthResult{false, "offline", {}, true, 0}; });
    CHECK(offline.outcome == Renewal::Failed);
    const SessionTokens::RenewResult busy =
        tokens.renew("A1", [](const std::string&) { return AuthResult{false, "busy", {}, false, 503}; });
    CHECK(busy.outcome == Renewal::Failed);
    CHECK(tokens.current("A1") == "A1");
    const SessionTokens::RenewResult refused =
        tokens.renew("A1", [](const std::string&) { return AuthResult{false, "refused", {}, false, 401}; });
    CHECK(refused.outcome == Renewal::Rejected);
    CHECK(!tokens.takeRenewed());
}

void earlierSessionIsNeverRenewed() {
    SessionTokens tokens;
    tokens.set(session("A1", "R1"));
    tokens.renew("A1", [](const std::string&) { return granted("A2", "R2"); });
    tokens.set(session("B1", "S1"));
    int calls = 0;
    const auto refresher = [&](const std::string&) {
        ++calls;
        return granted("B2", "S2");
    };
    CHECK(tokens.current("A1") == "A1" && tokens.current("A2") == "A2");
    CHECK(tokens.renew("A2", refresher).outcome == Renewal::Failed && calls == 0);
    CHECK(!tokens.takeRenewed());

    tokens.clear();
    CHECK(tokens.renew("B1", refresher).outcome == Renewal::Failed && calls == 0);
    CHECK(tokens.current("B1") == "B1");
}

void signOutDuringRefreshDropsTheResult() {
    SessionTokens tokens;
    tokens.set(session("A1", "R1"));
    const SessionTokens::RenewResult result = tokens.renew("A1", [&](const std::string&) {
        tokens.clear();
        return granted("A2", "R2");
    });
    CHECK(result.outcome == Renewal::Failed);
    CHECK(!tokens.takeRenewed());
    CHECK(tokens.current("A1") == "A1");
}
}

int main() {
    renewMapsStaleCopies();
    concurrentExpiryRefreshesOnce();
    refusalAndNetworkErrors();
    earlierSessionIsNeverRenewed();
    signOutDuringRefreshDropsTheResult();
    std::printf(failures == 0 ? "session_tokens_tests: ok\n" : "session_tokens_tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
