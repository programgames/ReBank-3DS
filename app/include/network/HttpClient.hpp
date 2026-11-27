#pragma once

#include "network/ApiTypes.hpp"
#include "network/ServerClock.hpp"
#include "network/SessionTokens.hpp"

#include <3ds.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

struct HttpResponse {
    bool success = false;
    std::uint32_t status = 0;
    std::string body;
    std::string message;
    bool sent = false;
    std::uint32_t retryAfterSeconds = 0;

    RequestOutcome failureOutcome() const { return sent ? RequestOutcome::Unknown : RequestOutcome::Refused; }
};

enum class HttpMethod { Get, Post, Put, Delete };

class HttpClient {
public:
    static constexpr std::uint64_t DefaultTimeoutNanoseconds = 30'000'000'000ULL;
    static constexpr std::uint64_t UploadTimeoutNanoseconds = 150'000'000'000ULL;

    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse send(HttpMethod method, const std::string& path, const std::string& body = {},
                      const std::string& accessToken = {},
                      std::uint64_t timeoutNanoseconds = DefaultTimeoutNanoseconds);
    FileDownloadResult download(const std::string& path, const std::string& destination, std::uint32_t expectedSize,
                                std::atomic<std::uint32_t>& progress, const std::string& accessToken = {});
    bool consumeSessionRejected();
    SessionTokens& session() { return session_; }
    // Set once at startup, before any worker runs.
    void setRefresher(SessionTokens::Refresher refresher) { refresher_ = std::move(refresher); }

private:
    HttpResponse sendRetrying(HttpMethod method, const std::string& path, const std::string& body,
                              const std::string& accessToken, std::uint64_t timeoutNanoseconds);
    HttpResponse sendOnce(HttpMethod method, const std::string& path, const std::string& body,
                          const std::string& accessToken, std::uint64_t timeoutNanoseconds);
    void syncClock(bool force = false);

    bool initialized_ = false;
    std::atomic<bool> sessionRejected_{false};
    ServerClock clock_;
    SessionTokens session_;
    SessionTokens::Refresher refresher_;
};
