#ifdef APC_DEVICE_OPERATION_COORDINATOR_STANDALONE
#include <limits>
#include <utility>
#else
#include <pch.h>
#endif

#include <core/DeviceOperationCoordinator.hpp>

namespace apc::device_operation {

std::optional<Token> DeviceOperationCoordinator::TryBegin(std::wstring_view deviceId, Intent intent, Phase phase) {
    if (deviceId.empty() || m_lastTokenId == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    if (m_active.contains(deviceId)) return std::nullopt;

    auto const tokenId = m_lastTokenId + 1;
    Token token{std::wstring(deviceId), tokenId, intent};
    auto [entry, inserted] = m_active.emplace(token.DeviceId, State{tokenId, intent, phase});
    (void)entry;
    if (!inserted) return std::nullopt;
    m_lastTokenId = tokenId;
    return token;
}

bool DeviceOperationCoordinator::Transition(Token const& token, Phase expected, Phase next) {
    auto state = m_active.find(token.DeviceId);
    if (state == m_active.end() || state->second.TokenId != token.Id ||
        state->second.OperationIntent != token.OperationIntent || state->second.OperationPhase != expected ||
        state->second.FailureReportClaimed) {
        return false;
    }

    state->second.OperationPhase = next;
    return true;
}

bool DeviceOperationCoordinator::TryClaimFailureReport(Token const& token) noexcept {
    auto state = m_active.find(token.DeviceId);
    if (state == m_active.end() || state->second.TokenId != token.Id ||
        state->second.OperationIntent != token.OperationIntent || state->second.FailureReportClaimed) {
        return false;
    }

    state->second.FailureReportClaimed = true;
    return true;
}

bool DeviceOperationCoordinator::Complete(Token const& token) {
    auto state = m_active.find(token.DeviceId);
    if (state == m_active.end() || state->second.TokenId != token.Id ||
        state->second.OperationIntent != token.OperationIntent) {
        return false;
    }

    m_active.erase(state);
    return true;
}

bool DeviceOperationCoordinator::Invalidate(std::wstring_view deviceId) {
    auto state = m_active.find(deviceId);
    if (state == m_active.end()) return false;
    m_active.erase(state);
    return true;
}

void DeviceOperationCoordinator::CancelAll() noexcept {
    m_active.clear();
}

bool DeviceOperationCoordinator::IsCurrent(Token const& token) const {
    auto state = m_active.find(token.DeviceId);
    return state != m_active.end() && state->second.TokenId == token.Id &&
           state->second.OperationIntent == token.OperationIntent;
}

bool DeviceOperationCoordinator::IsActive(std::wstring_view deviceId) const {
    return m_active.contains(deviceId);
}

bool DeviceOperationCoordinator::IsInPhase(std::wstring_view deviceId, Phase phase) const {
    auto state = m_active.find(deviceId);
    return state != m_active.end() && state->second.OperationPhase == phase;
}

bool DeviceOperationCoordinator::HasActiveOperations() const noexcept {
    return !m_active.empty();
}

std::vector<ActiveOperation> DeviceOperationCoordinator::Snapshot() const {
    std::vector<ActiveOperation> result;
    result.reserve(m_active.size());
    for (auto const& [deviceId, state] : m_active) {
        result.push_back(ActiveOperation{deviceId, state.OperationPhase});
    }
    return result;
}

} // namespace apc::device_operation
