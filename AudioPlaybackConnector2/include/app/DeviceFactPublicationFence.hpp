#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace apc::app {

// DeviceService facts can arrive while their UI presentation is queued. The controller owns this fence
// and advances it when normalizing facts; the UI consumer verifies its token before presentation.
// Equal facts deliberately share a generation so duplicate
// source notifications retain their current behavior. It never calls user code while holding its mutex.
class DeviceFactPublicationFence {
public:
    enum class Status { None, Ready, Connecting, Reconnecting, Connected, Error, WaitingForReconnect };

    struct Token {
        enum class Channel { Connection, Status };

        std::wstring DeviceId;
        std::uint64_t Generation = 0;
        Channel FactChannel = Channel::Connection;
    };

    struct Observation {
        bool WasConnected = false;
        bool WasWaitingForReconnect = false;
        Token Connection;
        Token State;
    };

    [[nodiscard]] Observation Observe(std::wstring_view deviceId, Status status) {
        std::scoped_lock lock(m_mutex);
        auto& state = m_devices[std::wstring(deviceId)];
        const bool wasConnected = state.IsConnected;
        const bool wasWaiting = state.CurrentStatus == Status::WaitingForReconnect;
        UpdateConnection(state, status == Status::Connected);
        UpdateStatus(state, status);
        return {wasConnected,
                wasWaiting,
                {std::wstring(deviceId), state.ConnectionGeneration, Token::Channel::Connection},
                {std::wstring(deviceId), state.StatusGeneration, Token::Channel::Status}};
    }

    [[nodiscard]] bool IsCurrent(Token const& token) const {
        std::scoped_lock lock(m_mutex);
        const auto found = m_devices.find(token.DeviceId);
        if (found == m_devices.end() || found->second.IsGenerationExhausted) return false;
        return token.FactChannel == Token::Channel::Connection ? found->second.ConnectionGeneration == token.Generation
                                                               : found->second.StatusGeneration == token.Generation;
    }

private:
    struct DeviceState {
        bool IsConnected = false;
        Status CurrentStatus = Status::None;
        std::uint64_t ConnectionGeneration = 0;
        std::uint64_t StatusGeneration = 0;
        bool IsGenerationExhausted = false;
    };

    static void UpdateConnection(DeviceState& state, bool isConnected) noexcept {
        if (state.IsConnected == isConnected) return;
        state.IsConnected = isConnected;
        AdvanceGeneration(state, state.ConnectionGeneration);
    }

    static void UpdateStatus(DeviceState& state, Status status) noexcept {
        if (state.CurrentStatus == status) return;
        state.CurrentStatus = status;
        AdvanceGeneration(state, state.StatusGeneration);
    }

    static void AdvanceGeneration(DeviceState& state, std::uint64_t& generation) noexcept {
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            state.IsGenerationExhausted = true;
            return;
        }
        ++generation;
    }

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, DeviceState> m_devices;
};

} // namespace apc::app
