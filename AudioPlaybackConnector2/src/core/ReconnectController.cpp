#ifdef APC_RECONNECT_CONTROLLER_STANDALONE
#include <algorithm>
#include <ranges>
#include <string>
#else
#include <pch.h>
#endif

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

std::wstring DeviceKey(std::wstring_view deviceId) {
    return std::wstring(deviceId);
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
    ++m_globalGeneration;
    m_allReconnectsCancelled = true;
    for (auto& [id, state] : m_states) {
        (void)id;
        ++state.Generation;
        state.CompletedAttempts = 0;
        state.TimerPending = false;
        state.AttemptInProgress = false;
        state.UserCancelled = true;
        state.FailureNotified = false;
    }
}

void ReconnectController::ClearTracking() {
    ++m_globalGeneration;
    m_states.clear();
}

void ReconnectController::BeginManualOperation(std::wstring_view deviceId) {
    m_allReconnectsCancelled = false;
    auto& state = m_states[DeviceKey(deviceId)];
    ++state.Generation;
    state.CompletedAttempts = 0;
    state.TimerPending = false;
    state.AttemptInProgress = false;
    state.UserCancelled = false;
    state.FailureNotified = false;
}

void ReconnectController::CancelDevice(std::wstring_view deviceId) {
    auto& state = m_states[DeviceKey(deviceId)];
    ++state.Generation;
    state.CompletedAttempts = 0;
    state.TimerPending = false;
    state.AttemptInProgress = false;
    state.UserCancelled = true;
    state.FailureNotified = false;
}

void ReconnectController::CompleteConnectionSucceeded(std::wstring_view deviceId) {
    auto& state = m_states[DeviceKey(deviceId)];
    ++state.Generation;
    state.CompletedAttempts = 0;
    state.TimerPending = false;
    state.AttemptInProgress = false;
    state.UserCancelled = false;
    state.FailureNotified = false;
}

bool ReconnectController::IsCancelled(std::wstring_view deviceId) const {
    auto iter = m_states.find(DeviceKey(deviceId));
    return iter != m_states.end() && iter->second.UserCancelled;
}

bool ReconnectController::AllReconnectsCancelled() const {
    return m_allReconnectsCancelled;
}

std::size_t ReconnectController::Attempts(std::wstring_view deviceId) const {
    auto iter = m_states.find(DeviceKey(deviceId));
    return iter == m_states.end() ? 0 : iter->second.CompletedAttempts;
}

bool ReconnectController::HasPendingTimer(std::wstring_view deviceId) const {
    auto iter = m_states.find(DeviceKey(deviceId));
    return iter != m_states.end() && (iter->second.TimerPending || iter->second.AttemptInProgress);
}

bool ReconnectController::HasAttemptInProgress(std::wstring_view deviceId) const {
    auto iter = m_states.find(DeviceKey(deviceId));
    return iter != m_states.end() && iter->second.AttemptInProgress;
}

bool ReconnectController::HasPendingTimers() const {
    return std::ranges::any_of(
        m_states, [](auto const& entry) { return entry.second.TimerPending || entry.second.AttemptInProgress; });
}

std::vector<std::wstring> ReconnectController::PendingDeviceIds() const {
    std::vector<std::wstring> result;
    result.reserve(m_states.size());
    for (auto const& [id, state] : m_states) {
        if (state.TimerPending || state.AttemptInProgress) result.push_back(id);
    }
    return result;
}

ReconnectController::ScheduleDecision ReconnectController::PrepareSchedule(std::wstring_view deviceId, bool blocked) {
    const auto key = DeviceKey(deviceId);
    auto& state = m_states[key];
    ScheduleDecision decision;
    decision.MaxAttempts = c_maxReconnectAttempts;

    if (blocked || m_allReconnectsCancelled || state.UserCancelled || state.TimerPending || state.AttemptInProgress) {
        return decision;
    }

    if (state.CompletedAttempts >= c_maxReconnectAttempts) {
        if (!state.FailureNotified) {
            state.FailureNotified = true;
            decision.NotifyFailed = true;
        }
        return decision;
    }

    decision.ShouldSchedule = true;
    decision.Attempt = state.CompletedAttempts + 1;
    decision.Delay = GetReconnectDelay(decision.Attempt);
    decision.Token = TimerToken{key, m_globalGeneration, state.Generation, decision.Attempt};
    state.TimerPending = true;
    return decision;
}

bool ReconnectController::ClaimTimer(TimerToken const& token, bool blocked) {
    auto iter = m_states.find(token.DeviceId);
    if (blocked || m_allReconnectsCancelled || token.GlobalGeneration != m_globalGeneration || iter == m_states.end()) {
        return false;
    }

    auto& state = iter->second;
    if (token.DeviceGeneration != state.Generation || token.Attempt != state.CompletedAttempts + 1 ||
        !state.TimerPending || state.AttemptInProgress || state.UserCancelled) {
        return false;
    }

    state.TimerPending = false;
    state.AttemptInProgress = true;
    return true;
}

ReconnectController::ScheduleDecision ReconnectController::CompleteAttemptFailed(TimerToken const& token) {
    ScheduleDecision decision;
    decision.MaxAttempts = c_maxReconnectAttempts;
    auto iter = m_states.find(token.DeviceId);
    if (token.GlobalGeneration != m_globalGeneration || iter == m_states.end()) return decision;

    auto& state = iter->second;
    if (token.DeviceGeneration != state.Generation || token.Attempt != state.CompletedAttempts + 1 ||
        !state.AttemptInProgress) {
        return decision;
    }

    state.AttemptInProgress = false;
    state.CompletedAttempts = token.Attempt;
    decision.AttemptCompleted = true;
    if (state.CompletedAttempts >= c_maxReconnectAttempts && !state.FailureNotified) {
        state.FailureNotified = true;
        decision.NotifyFailed = true;
    }
    return decision;
}

void ReconnectController::CompleteAttemptSucceeded(TimerToken const& token) {
    auto iter = m_states.find(token.DeviceId);
    if (token.GlobalGeneration != m_globalGeneration || iter == m_states.end()) return;

    auto& state = iter->second;
    if (token.DeviceGeneration != state.Generation || !state.AttemptInProgress) return;

    ++state.Generation;
    state.CompletedAttempts = 0;
    state.TimerPending = false;
    state.AttemptInProgress = false;
    state.UserCancelled = false;
    state.FailureNotified = false;
}

void ReconnectController::HandleTimerCreateFailed(TimerToken const& token) {
    auto iter = m_states.find(token.DeviceId);
    if (token.GlobalGeneration != m_globalGeneration || iter == m_states.end()) return;

    auto& state = iter->second;
    if (token.DeviceGeneration == state.Generation && token.Attempt == state.CompletedAttempts + 1) {
        state.TimerPending = false;
    }
}
