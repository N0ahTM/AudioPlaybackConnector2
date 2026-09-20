#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Adaptive Schedule State ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class AdaptiveScheduleState {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] std::uint64_t Supersede() noexcept;
    [[nodiscard]] bool SetWin32NotBefore(std::uint64_t generation, TimePoint notBefore) noexcept;
    [[nodiscard]] bool Consume(std::uint64_t generation) noexcept;
    [[nodiscard]] bool ConsumeWin32IfDue(TimePoint now) noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::uint64_t m_generation = 0;
    std::optional<TimePoint> m_win32NotBefore;
    bool m_active = false;
};
