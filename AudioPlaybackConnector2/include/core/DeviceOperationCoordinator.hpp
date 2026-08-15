#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace apc::device_operation {

enum class Intent { ManualConnect, ManualReconnect, AutoReconnect, IncomingEnable };

enum class Phase { Connecting, Reconnecting, EnablingIncoming };

struct Token {
    std::wstring DeviceId;
    std::uint64_t Id = 0;
    Intent OperationIntent = Intent::ManualConnect;

    bool operator==(Token const&) const = default;
};

struct ActiveOperation {
    std::wstring DeviceId;
    Phase OperationPhase = Phase::Connecting;
};

class DeviceOperationCoordinator {
public:
    [[nodiscard]] std::optional<Token> TryBegin(std::wstring_view deviceId, Intent intent, Phase phase);
    [[nodiscard]] bool Transition(Token const& token, Phase expected, Phase next);
    [[nodiscard]] bool TryClaimFailureReport(Token const& token) noexcept;
    [[nodiscard]] bool Complete(Token const& token);
    [[nodiscard]] bool Invalidate(std::wstring_view deviceId);
    void CancelAll() noexcept;

    [[nodiscard]] bool IsCurrent(Token const& token) const;
    [[nodiscard]] bool IsActive(std::wstring_view deviceId) const;
    [[nodiscard]] bool IsInPhase(std::wstring_view deviceId, Phase phase) const;
    [[nodiscard]] bool HasActiveOperations() const noexcept;
    [[nodiscard]] std::vector<ActiveOperation> Snapshot() const;

#ifdef APC_DEVICE_OPERATION_COORDINATOR_TESTING
    void SetLastTokenIdForTesting(std::uint64_t value) noexcept { m_lastTokenId = value; }
#endif

private:
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::wstring_view value) const noexcept {
            return std::hash<std::wstring_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(std::wstring const& value) const noexcept {
            return (*this)(std::wstring_view(value));
        }
    };

    struct State {
        std::uint64_t TokenId = 0;
        Intent OperationIntent = Intent::ManualConnect;
        Phase OperationPhase = Phase::Connecting;
        bool FailureReportClaimed = false;
    };

    std::unordered_map<std::wstring, State, TransparentStringHash, std::equal_to<>> m_active;
    std::uint64_t m_lastTokenId = 0;
};

} // namespace apc::device_operation
