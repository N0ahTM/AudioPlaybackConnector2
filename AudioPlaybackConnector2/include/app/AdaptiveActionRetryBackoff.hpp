#pragma once

#include <chrono>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Adaptive Action Retry Backoff /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class AdaptiveActionRetryBackoff {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit AdaptiveActionRetryBackoff(std::chrono::milliseconds initialDelay = std::chrono::seconds{1},
                                        std::chrono::milliseconds maximumDelay = std::chrono::seconds{30}) noexcept;

    [[nodiscard]] std::chrono::milliseconds RecordFailure() noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::chrono::milliseconds CurrentDelay() const noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::chrono::milliseconds m_initialDelay;
    std::chrono::milliseconds m_maximumDelay;
    std::chrono::milliseconds m_currentDelay;
};
