#include <core/ReconnectPolicy.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestFullBackoffSequence() {
    constexpr std::array expectedDelays{5, 10, 20, 40, 60, 60, 60, 60, 60, 60};

    for (std::size_t index = 0; index < expectedDelays.size(); ++index) {
        const ReconnectPolicyInput input{
            .Request = ReconnectRequest::ConnectionLoss,
            .CompletedAttempts = index,
        };
        const auto decision = ReconnectPolicy::Evaluate(input);
        const auto repeated = ReconnectPolicy::Evaluate(input);
        Check(decision.Kind == ReconnectDecisionKind::Retry, "an incomplete connection-loss sequence must retry");
        Check(decision.Attempt == index + 1, "the next attempt must follow completed failures");
        Check(decision.MaxAttempts == ReconnectPolicy::MaxAttempts, "the retry limit must be part of the result");
        Check(decision.Delay == std::chrono::seconds(expectedDelays[index]),
              "the retry delay must use the bounded exponential sequence");
        Check(decision.Kind == repeated.Kind && decision.Attempt == repeated.Attempt &&
                  decision.Delay == repeated.Delay,
              "the same value input must produce the same decision");
    }
}

void TestBlockedDeferralPreservesAttempt() {
    const ReconnectPolicyInput blockedInput{
        .Request = ReconnectRequest::ConnectionLoss,
        .CompletedAttempts = 4,
        .IsBlocked = true,
    };
    const auto deferred = ReconnectPolicy::Evaluate(blockedInput);
    Check(deferred.Kind == ReconnectDecisionKind::Defer, "a blocked reconnect must be deferred");
    Check(deferred.Attempt == 5, "deferral must preserve the next attempt number");
    Check(deferred.Delay == ReconnectPolicy::BlockedRetryDelay, "blocked reconnects must use the fixed deferral delay");

    auto unblockedInput = blockedInput;
    unblockedInput.IsBlocked = false;
    const auto retry = ReconnectPolicy::Evaluate(unblockedInput);
    Check(retry.Kind == ReconnectDecisionKind::Retry, "an unblocked reconnect must return to retry scheduling");
    Check(retry.Attempt == deferred.Attempt, "deferral must not consume a retry attempt");
    Check(retry.Delay == ReconnectPolicy::MaximumRetryDelay, "unblocked retry must resume the normal attempt backoff");

    const auto repeated = ReconnectPolicy::Evaluate(blockedInput);
    Check(repeated.Kind == deferred.Kind && repeated.Attempt == deferred.Attempt && repeated.Delay == deferred.Delay,
          "blocked deferral must be deterministic without policy state");
}

void TestCancellationReasonsDoNotBecomeRetries() {
    const auto userCancelled = ReconnectPolicy::Evaluate({
        .Request = ReconnectRequest::UserCancellation,
    });
    Check(userCancelled.Kind == ReconnectDecisionKind::UserCancelled,
          "user cancellation must remain distinct from connection-loss retry");
    Check(userCancelled.Delay == std::chrono::seconds::zero(), "user cancellation must not schedule a timer");

    const auto disabled = ReconnectPolicy::Evaluate({
        .Request = ReconnectRequest::PolicyDisabled,
    });
    Check(disabled.Kind == ReconnectDecisionKind::DisabledByPolicy,
          "policy disable must remain distinct from user cancellation");
    Check(disabled.Delay == std::chrono::seconds::zero(), "policy disable must not schedule a timer");

    const auto loss = ReconnectPolicy::Evaluate({
        .Request = ReconnectRequest::ConnectionLoss,
    });
    Check(loss.Kind == ReconnectDecisionKind::Retry, "an ordinary connection loss must remain eligible for retry");
}

void TestTerminalFailureDecision() {
    const auto terminal = ReconnectPolicy::Evaluate({
        .Request = ReconnectRequest::ConnectionLoss,
        .CompletedAttempts = ReconnectPolicy::MaxAttempts,
    });
    Check(terminal.Kind == ReconnectDecisionKind::TerminalFailure,
          "the retry limit must produce a terminal-failure decision");
    Check(terminal.Attempt == ReconnectPolicy::MaxAttempts,
          "terminal failure must report the bounded number of completed attempts");
    Check(terminal.Delay == std::chrono::seconds::zero(), "terminal failure must not schedule a timer");

    const auto beyondLimit = ReconnectPolicy::Evaluate({
        .Request = ReconnectRequest::ConnectionLoss,
        .CompletedAttempts = ReconnectPolicy::MaxAttempts + 1,
    });
    Check(beyondLimit.Kind == ReconnectDecisionKind::TerminalFailure,
          "attempt counts beyond the limit must remain terminal");
}

} // namespace

int RunReconnectPolicyTests() {
    TestFullBackoffSequence();
    TestBlockedDeferralPreservesAttempt();
    TestCancellationReasonsDoNotBecomeRetries();
    TestTerminalFailureDecision();
    return g_failures;
}

#ifdef APC_RECONNECT_POLICY_TEST_MAIN
int main() {
    return RunReconnectPolicyTests();
}
#endif
