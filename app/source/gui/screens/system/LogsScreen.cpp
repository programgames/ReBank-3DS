#include "gui/screens/system/LogsScreen.hpp"

#include "platform/Logger.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace Gui;

namespace {
constexpr std::size_t VisibleLines = 10;
constexpr float LineLeft = 12.0F;
constexpr float LineWidth = 320.0F - 2.0F * LineLeft;
constexpr float LineSize = 0.38F;

u32 colorFor(LogLevel level) {
    switch (level) {
        case LogLevel::Warning:
            return Accent;
        case LogLevel::Error:
            return Error;
        case LogLevel::Info:
            break;
    }
    return Ink;
}
}

void LogsScreen::update(const InputFrame& input) {
    const CursorDirection direction = directions_.next(input);
    if (direction == CursorDirection::Up) {
        ++scroll_;
    } else if (direction == CursorDirection::Down && scroll_ > 0) {
        --scroll_;
    }
}

void LogsScreen::render() {
    ui().drawText("ReBank Log", 12.0F, 10.0F, 0.62F, Ink);
    ui().drawText("SELECT", 253.0F, 13.0F, 0.38F, Brand);
    const std::vector<LogEntry> entries = Logger::instance().recent(scroll_ + VisibleLines);
    if (entries.empty()) {
        return;
    }
    scroll_ = std::min(scroll_, entries.size() - 1);
    // Fill the screen bottom-up from the newest shown entry, so only the visible entries get wrapped.
    std::vector<std::pair<std::string, u32>> lines;
    for (std::size_t index = entries.size() - scroll_; index > 0 && lines.size() < VisibleLines; --index) {
        const LogEntry& entry = entries[index - 1];
        const std::vector<std::string> wrapped = ui().wrap(entry.message, LineSize, LineWidth);
        for (auto line = wrapped.rbegin(); line != wrapped.rend() && lines.size() < VisibleLines; ++line) {
            lines.emplace_back(*line, colorFor(entry.level));
        }
    }
    for (std::size_t row = 0; row < lines.size(); ++row) {
        const auto& [line, color] = lines[lines.size() - 1 - row];
        ui().drawText(line, LineLeft, 39.0F + static_cast<float>(row) * 18.0F, LineSize, color);
    }
}
