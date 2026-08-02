#include <core/ReconnectController.hpp>

#include <array>
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
    constexpr std::wstring_view id = L"device-a";
    constexpr std::array expectedDelays{5, 10, 20, 40, 60, 60, 60, 60, 60, 60};
    ReconnectController controller;

    for (std::size_t index = 0; index < expectedDelays.size(); ++index) {
        auto decision = controller.PrepareSchedule(id, false);
        Check(decision.ShouldSchedule, "each incomplete attempt must schedule");
        Check(decision.Attempt == index + 1, "attempt number must advance only after a completed failure");
        Check(decision.Delay == std::chrono::seconds(expectedDelays[index]), "backoff delay must be deterministic");
        Check(controller.HasPendingTimer(id), "scheduled timer must be reported as pending");
        Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "a device may have only one pending timer");
        Check(controller.ClaimTimer(decision.Token, false), "the current timer token must be claimable exactly once");
        Check(!controller.ClaimTimer(decision.Token, false), "a claimed timer token must not be claimable twice");

        auto completion = controller.CompleteAttemptFailed(decision.Token);
        Check(completion.AttemptCompleted, "a claimed attempt failure must be accepted");
        Check(completion.NotifyFailed == (index + 1 == expectedDelays.size()),
              "terminal failure must be notified exactly on attempt ten");
        Check(controller.Attempts(id) == index + 1, "only completed attempts count toward the limit");
    }

    auto terminal = controller.PrepareSchedule(id, false);
    Check(!terminal.ShouldSchedule, "no timer may be scheduled after the terminal attempt");
    Check(!terminal.NotifyFailed, "terminal failure notification must be one-shot");
}

void TestSuccessAndStaleTokens() {
    constexpr std::wstring_view id = L"device-b";
    ReconnectController controller;

    auto first = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(first.Token, false), "first timer must be claimable");
    auto firstFailure = controller.CompleteAttemptFailed(first.Token);
    Check(firstFailure.AttemptCompleted, "first failure must complete");

    auto second = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(second.Token, false), "second timer must be claimable");
    controller.CompleteAttemptSucceeded(second.Token);
    Check(controller.Attempts(id) == 0, "success must reset attempts");
    Check(!controller.HasPendingTimer(id), "success must clear busy reconnect state");
    Check(!controller.CompleteAttemptFailed(second.Token).AttemptCompleted,
          "a success-invalidated token must not mutate state later");

    auto stale = controller.PrepareSchedule(id, false);
    controller.BeginManualOperation(id);
    Check(!controller.ClaimTimer(stale.Token, false), "manual operations must invalidate older automatic timers");
}

void TestCancellationAndTimerCreationFailure() {
    constexpr std::wstring_view id = L"device-c";
    ReconnectController controller;

    auto pending = controller.PrepareSchedule(id, false);
    controller.CancelDevice(id);
    Check(!controller.ClaimTimer(pending.Token, false), "device cancellation must invalidate a pending timer");
    Check(controller.IsCancelled(id), "device cancellation must remain observable");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "cancelled devices must not silently restart");

    controller.BeginManualOperation(id);
    auto retryable = controller.PrepareSchedule(id, false);
    controller.HandleTimerCreateFailed(retryable.Token);
    auto replacement = controller.PrepareSchedule(id, false);
    Check(replacement.ShouldSchedule, "timer creation failure must release the pending slot");
    Check(replacement.Attempt == 1, "timer creation failure must not consume an attempt");

    controller.CancelPendingReconnects();
    Check(!controller.ClaimTimer(replacement.Token, false), "global cancellation must invalidate every timer token");
    Check(controller.AllReconnectsCancelled(), "global cancellation must remain observable");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "global cancellation must block new timers");
}

void TestObservedConnectionInvalidatesAttempt() {
    constexpr std::wstring_view id = L"device-d";
    ReconnectController controller;

    auto decision = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(decision.Token, false), "timer must be claimable before observed success");
    controller.CompleteConnectionSucceeded(id);
    Check(!controller.CompleteAttemptFailed(decision.Token).AttemptCompleted,
          "an observed connection must invalidate a late failure completion");
    Check(controller.Attempts(id) == 0, "an observed connection must reset attempts");
}

} // namespace

int main() {
    TestFullBackoffSequence();
    TestSuccessAndStaleTokens();
    TestCancellationAndTimerCreationFailure();
    TestObservedConnectionInvalidatesAttempt();

    if (g_failures != 0) {
        std::cerr << g_failures << " reconnect controller test(s) failed\n";
        return 1;
    }
    std::cout << "All reconnect controller tests passed\n";
    return 0;
}
