#include <algorithm>
#include <core/ReconnectPolicy.hpp>

namespace {
std::chrono::seconds RetryDelayForAttempt(std::size_t attempt) noexcept {
    auto delay = ReconnectPolicy::InitialRetryDelay;
    for (std::size_t previousAttempt = 1; previousAttempt < attempt; ++previousAttempt) {
        delay = std::min(delay * 2, ReconnectPolicy::MaximumRetryDelay);
    }
    return delay;
}

} // namespace

ReconnectPolicyDecision ReconnectPolicy::Evaluate(ReconnectPolicyInput const& input) noexcept {
    if (input.Request == ReconnectRequest::UserCancellation) {
        return {.Kind = ReconnectDecisionKind::UserCancelled, .MaxAttempts = MaxAttempts};
    }
    if (input.Request == ReconnectRequest::PolicyDisabled) {
        return {.Kind = ReconnectDecisionKind::DisabledByPolicy, .MaxAttempts = MaxAttempts};
    }

    if (input.CompletedAttempts >= MaxAttempts) {
        return {
            .Kind = ReconnectDecisionKind::TerminalFailure,
            .Attempt = MaxAttempts,
            .MaxAttempts = MaxAttempts,
        };
    }

    const auto attempt = input.CompletedAttempts + 1;
    if (input.IsBlocked) {
        return {
            .Kind = ReconnectDecisionKind::Defer,
            .Attempt = attempt,
            .MaxAttempts = MaxAttempts,
            .Delay = BlockedRetryDelay,
        };
    }

    return {
        .Kind = ReconnectDecisionKind::Retry,
        .Attempt = attempt,
        .MaxAttempts = MaxAttempts,
        .Delay = RetryDelayForAttempt(attempt),
    };
}
