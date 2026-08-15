#pragma once

#include <chrono>
#include <optional>

class AutomaticUpdateWindow {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] bool Update(bool allowed, bool stopping, TimePoint now, Clock::duration stableDelay) noexcept;
    [[nodiscard]] Clock::duration Remaining(TimePoint now, Clock::duration stableDelay) const noexcept;
    void Reset() noexcept;

private:
    std::optional<TimePoint> m_allowedSince;
};
