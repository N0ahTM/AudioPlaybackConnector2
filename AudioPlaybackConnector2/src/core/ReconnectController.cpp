#include <pch.h>

#include <core/ReconnectController.hpp>

namespace {
constexpr std::size_t c_maxReconnectAttempts = 10;
constexpr int c_initialReconnectDelaySeconds = 5;
constexpr int c_maxReconnectDelaySeconds = 60;
constexpr std::chrono::milliseconds c_reconnectCloseCooldown{1500};

std::chrono::seconds GetReconnectDelay(std::size_t attempt) {
    auto delay = c_initialReconnectDelaySeconds;
    for (std::size_t i = 1; i < attempt && delay < c_maxReconnectDelaySeconds; ++i) {
        delay = std::min(delay * 2, c_maxReconnectDelaySeconds);
    }
    return std::chrono::seconds(delay);
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::chrono::milliseconds ReconnectController::ReconnectCloseCooldown() {
    return c_reconnectCloseCooldown;
}

void ReconnectController::AllowReconnects() {
    m_allReconnectsCancelled = false;
}

void ReconnectController::CancelPendingReconnects() {
    m_allReconnectsCancelled = true;
    m_reconnectAttempts.clear();
    m_cancelledReconnectIds.clear();
    DebugTrace(L"[ReconnectController] Pending reconnects cancelled");
}

void ReconnectController::ClearTracking() {
    m_reconnectAttempts.clear();
    m_cancelledReconnectIds.clear();
    m_reconnectTimerCounts.clear();
}

void ReconnectController::BeginManualReconnect(winrt::hstring const& deviceId) {
    m_allReconnectsCancelled = false;
    m_cancelledReconnectIds.erase(deviceId);
    m_reconnectAttempts.erase(deviceId);
}

void ReconnectController::ClearAttempts(winrt::hstring const& deviceId) {
    m_reconnectAttempts.erase(deviceId);
}

void ReconnectController::MarkUserCancelled(winrt::hstring const& deviceId) {
    m_cancelledReconnectIds.insert(deviceId);
    m_reconnectAttempts.erase(deviceId);
}

void ReconnectController::ClearUserCancellationIfNoTimer(winrt::hstring const& deviceId) {
    if (m_reconnectTimerCounts[deviceId] == 0) {
        m_reconnectTimerCounts.erase(deviceId);
        m_cancelledReconnectIds.erase(deviceId);
    }
}

void ReconnectController::ClearCancelled(winrt::hstring const& deviceId) {
    m_cancelledReconnectIds.erase(deviceId);
}

bool ReconnectController::IsCancelled(winrt::hstring const& deviceId) const {
    return m_cancelledReconnectIds.count(deviceId) > 0;
}

bool ReconnectController::AllReconnectsCancelled() const {
    return m_allReconnectsCancelled;
}

std::size_t ReconnectController::Attempts(winrt::hstring const& deviceId) const {
    if (auto iter = m_reconnectAttempts.find(deviceId); iter != m_reconnectAttempts.end()) {
        return iter->second;
    }
    return 0;
}

bool ReconnectController::HasPendingTimer(winrt::hstring const& deviceId) const {
    auto iter = m_reconnectTimerCounts.find(deviceId);
    return iter != m_reconnectTimerCounts.end() && iter->second > 0;
}

bool ReconnectController::HasPendingTimers() const {
    return std::ranges::any_of(m_reconnectTimerCounts, [](auto const& entry) { return entry.second > 0; });
}

ReconnectController::ScheduleDecision ReconnectController::PrepareSchedule(winrt::hstring const& deviceId,
                                                                           bool blocked) {
    ScheduleDecision decision;
    decision.MaxAttempts = c_maxReconnectAttempts;

    if (blocked) {
        m_cancelledReconnectIds.insert(deviceId);
        return decision;
    }

    auto& attempts = m_reconnectAttempts[deviceId];
    if (attempts >= c_maxReconnectAttempts) {
        decision.NotifyFailed = attempts == c_maxReconnectAttempts;
        attempts = c_maxReconnectAttempts + 1;
        m_cancelledReconnectIds.insert(deviceId);
        return decision;
    }

    decision.ShouldSchedule = true;
    decision.Attempt = ++attempts;
    decision.Delay = GetReconnectDelay(decision.Attempt);
    return decision;
}

void ReconnectController::StartTimer(winrt::hstring const& deviceId) {
    m_cancelledReconnectIds.erase(deviceId);
    ++m_reconnectTimerCounts[deviceId];
}

bool ReconnectController::ShouldSkipTimer(winrt::hstring const& deviceId, bool shutdownForProcessExit) const {
    return shutdownForProcessExit || m_allReconnectsCancelled || m_cancelledReconnectIds.count(deviceId) > 0;
}

void ReconnectController::CompleteTimer(winrt::hstring const& deviceId) {
    auto iter = m_reconnectTimerCounts.find(deviceId);
    if (iter == m_reconnectTimerCounts.end()) return;

    if (iter->second > 1) {
        --iter->second;
    } else {
        m_reconnectTimerCounts.erase(iter);
        m_cancelledReconnectIds.erase(deviceId);
    }
}

void ReconnectController::HandleTimerCreateFailed(winrt::hstring const& deviceId) {
    auto iter = m_reconnectTimerCounts.find(deviceId);
    if (iter == m_reconnectTimerCounts.end()) return;

    if (iter->second > 1) {
        --iter->second;
    } else {
        m_reconnectTimerCounts.erase(iter);
    }
}
