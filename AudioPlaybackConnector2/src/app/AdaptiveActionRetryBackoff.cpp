#include <app/AdaptiveActionRetryBackoff.hpp>

#include <algorithm>

AdaptiveActionRetryBackoff::AdaptiveActionRetryBackoff(std::chrono::milliseconds initialDelay,
                                                       std::chrono::milliseconds maximumDelay) noexcept
    : m_initialDelay(std::max(initialDelay, std::chrono::milliseconds{1})),
      m_maximumDelay(std::max(maximumDelay, m_initialDelay)), m_currentDelay(m_initialDelay) {}

std::chrono::milliseconds AdaptiveActionRetryBackoff::RecordFailure() noexcept {
    auto const delay = m_currentDelay;
    if (m_currentDelay >= m_maximumDelay) return delay;

    auto const remaining = m_maximumDelay - m_currentDelay;
    m_currentDelay = remaining < m_currentDelay ? m_maximumDelay : m_currentDelay * 2;
    return delay;
}

void AdaptiveActionRetryBackoff::Reset() noexcept {
    m_currentDelay = m_initialDelay;
}

std::chrono::milliseconds AdaptiveActionRetryBackoff::CurrentDelay() const noexcept {
    return m_currentDelay;
}
