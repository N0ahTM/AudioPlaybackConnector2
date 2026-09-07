#pragma once

#include <chrono>
#include <cstddef>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Reconnect Policy //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class ReconnectRequest {
    ConnectionLoss,
    Startup,
    Resume,
    UserCancellation,
    PolicyDisabled,
};

enum class ReconnectDecisionKind {
    Retry,
    Defer,
    UserCancelled,
    DisabledByPolicy,
    TerminalFailure,
};

struct ReconnectPolicyInput {
    ReconnectRequest Request = ReconnectRequest::ConnectionLoss;
    std::size_t CompletedAttempts = 0;
    bool IsBlocked = false;
};

struct ReconnectPolicyDecision {
    ReconnectDecisionKind Kind = ReconnectDecisionKind::TerminalFailure;
    std::size_t Attempt = 0;
    std::size_t MaxAttempts = 0;
    std::chrono::seconds Delay{};
};

class ReconnectPolicy {
public:
    static constexpr std::size_t MaxAttempts = 10;
    static constexpr std::chrono::seconds InitialRetryDelay{5};
    static constexpr std::chrono::seconds BlockedRetryDelay{5};
    static constexpr std::chrono::seconds MaximumRetryDelay{60};

    ReconnectPolicy() = delete;

    // The caller owns attempt, timer, generation, and one-shot notification state.
    [[nodiscard]] static ReconnectPolicyDecision Evaluate(ReconnectPolicyInput const& input) noexcept;
};
