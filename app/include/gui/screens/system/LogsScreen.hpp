#pragma once

#include "bank/selection/DirectionRepeat.hpp"
#include "gui/Screen.hpp"

#include <cstddef>

class LogsScreen : public Screen {
public:
    using Screen::Screen;

    void update(const InputFrame& input) override;
    void render() override;

private:
    DirectionRepeat directions_;
    std::size_t scroll_ = 0; // entries hidden below the bottom line; 0 follows the newest entry
};
