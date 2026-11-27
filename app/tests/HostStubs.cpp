// Host versions of the platform classes that need libctru: no lock, and a logger that drops
// everything.

#include "platform/FsGuard.hpp"
#include "platform/Logger.hpp"

FsGuard::FsGuard() {}
FsGuard::~FsGuard() {}

Logger::Logger() {}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::info(std::string_view) {}
void Logger::warning(std::string_view) {}
void Logger::error(std::string_view) {}
