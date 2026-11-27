#include "bank/selection/DirectionRepeat.hpp"

#include "platform/Clock.hpp"

#include <cstdlib>

namespace {
constexpr double InitialRepeatDelaySeconds = 0.28;
constexpr double RepeatIntervalSeconds = 0.09;

CursorDirection keysDirection(u32 keys) {
    if (keys & KEY_UP) {
        return CursorDirection::Up;
    }
    if (keys & KEY_DOWN) {
        return CursorDirection::Down;
    }
    if (keys & KEY_LEFT) {
        return CursorDirection::Left;
    }
    return keys & KEY_RIGHT ? CursorDirection::Right : CursorDirection::None;
}

// The dominant axis wins, so a diagonal stick still moves a menu that has only one axis.
CursorDirection directionFromCircle(circlePosition circle) {
    constexpr s16 Threshold = InputFrame::StickThreshold;
    if (std::abs(circle.dy) >= std::abs(circle.dx)) {
        if (circle.dy > Threshold) {
            return CursorDirection::Up;
        }
        return circle.dy < -Threshold ? CursorDirection::Down : CursorDirection::None;
    }
    if (circle.dx < -Threshold) {
        return CursorDirection::Left;
    }
    return circle.dx > Threshold ? CursorDirection::Right : CursorDirection::None;
}
}

CursorDirection DirectionRepeat::fromKeys(u32 keys) {
    return keysDirection(keys);
}

CursorDirection DirectionRepeat::next(const InputFrame& input) {
    // D-pad first; the circle pad's own key bits are left to directionFromCircle.
    CursorDirection direction = fromKeys(input.held & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT));
    if (direction == CursorDirection::None) {
        direction = directionFromCircle(input.circle);
    }
    if (direction == CursorDirection::None) {
        held_ = CursorDirection::None;
        repeatAt_ = 0;
        return CursorDirection::None;
    }
    if (direction != held_ || fromKeys(input.down) == direction) {
        held_ = direction;
        repeatAt_ = Clock::after(InitialRepeatDelaySeconds);
        return direction;
    }
    if (Clock::reached(repeatAt_)) {
        repeatAt_ = Clock::after(RepeatIntervalSeconds);
        return direction;
    }
    return CursorDirection::None;
}
