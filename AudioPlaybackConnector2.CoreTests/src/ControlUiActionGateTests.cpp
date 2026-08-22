#include <app/ControlUiActionGate.hpp>
#include <app/AppModels.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <stop_token>
#include <thread>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckCommand(bool condition, std::string_view command, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << command << ": " << message << '\n';
}

enum class ContextTermination { Cancellation, Deadline };

struct TerminationContext {
    std::stop_source StopSource;
    apc::app::AppCommandContext Context;

    explicit TerminationContext(ContextTermination termination)
        : Context{StopSource.get_token(), apc::app::AppCommandContext::TimePoint::max()} {
        if (termination == ContextTermination::Cancellation) {
            StopSource.request_stop();
        } else {
            Context.Deadline = apc::app::AppCommandContext::TimePoint::min();
        }
    }

    [[nodiscard]] bool IsTriggered(ContextTermination termination) const noexcept {
        return termination == ContextTermination::Cancellation
                   ? Context.IsCancellationRequested()
                   : Context.IsExpired(apc::app::AppCommandContext::Clock::now());
    }
};

ControlUiActionGate::Result
ClassifyAfterTermination(ControlUiActionGate& gate, TerminationContext const& context, ContextTermination termination) {
    return context.IsTriggered(termination) ? gate.CancelOrClassify() : gate.CurrentResult();
}

void TestCancellationBeforeDispatch() {
    ControlUiActionGate gate;
    Check(gate.CancelOrClassify() == ControlUiActionGate::Result::Failed,
          "a pending UI action must be cancellable with a definite failure");
    Check(!gate.TryBegin(), "a cancelled UI action must never begin later on the dispatcher");
    Check(gate.CurrentPhase() == ControlUiActionGate::Phase::Cancelled,
          "pre-dispatch cancellation must remain terminal");
}

void TestTimeoutAfterDispatchStarted() {
    ControlUiActionGate gate;
    Check(gate.TryBegin(), "a pending UI action must begin exactly once");
    Check(gate.CancelOrClassify() == ControlUiActionGate::Result::Indeterminate,
          "timeout after execution begins must never report a definite failure");
    gate.Complete(true);
    Check(gate.CurrentResult() == ControlUiActionGate::Result::Succeeded,
          "a running UI action may complete successfully after the caller times out");
}

void TestCancellationAndDeadlinePreserveDispatchClassificationForBothUiCommands() {
    constexpr std::array<std::string_view, 2> commands{"show", "settings"};
    constexpr std::array<ContextTermination, 2> terminations{ContextTermination::Cancellation,
                                                             ContextTermination::Deadline};

    for (const auto command : commands) {
        for (const auto termination : terminations) {
            TerminationContext beforeDispatch(termination);
            CheckCommand(beforeDispatch.IsTriggered(termination),
                         command,
                         "the deterministic cancellation/deadline context must be triggered");
            ControlUiActionGate pending;
            const auto beforeResult = ClassifyAfterTermination(pending, beforeDispatch, termination);
            CheckCommand(beforeResult == ControlUiActionGate::Result::Failed,
                         command,
                         "cancellation/deadline before TryBegin must remain a definite failure");
            CheckCommand(!pending.TryBegin(), command, "a pre-dispatch cancellation must prevent the queued UI work");

            TerminationContext afterDispatch(termination);
            CheckCommand(afterDispatch.IsTriggered(termination),
                         command,
                         "the deterministic post-dispatch context must be triggered");
            ControlUiActionGate running;
            CheckCommand(running.TryBegin(), command, "the UI action must cross TryBegin before termination");
            const auto afterResult = ClassifyAfterTermination(running, afterDispatch, termination);
            CheckCommand(afterResult == ControlUiActionGate::Result::Indeterminate,
                         command,
                         "cancellation/deadline after TryBegin must remain indeterminate");
            running.Complete(true);
            CheckCommand(running.CurrentResult() == ControlUiActionGate::Result::Succeeded,
                         command,
                         "in-flight UI work may complete after the caller receives an indeterminate result");
        }
    }
}

void TestCompletionResults() {
    ControlUiActionGate success;
    Check(success.TryBegin(), "successful action must begin");
    success.Complete(true);
    Check(success.CurrentResult() == ControlUiActionGate::Result::Succeeded,
          "successful completion must be observable");

    ControlUiActionGate failure;
    Check(failure.TryBegin(), "failing action must begin");
    failure.Complete(false);
    Check(failure.CurrentResult() == ControlUiActionGate::Result::Failed, "failing completion must be observable");
}

void TestBeginCancelRace() {
    for (int iteration = 0; iteration < 10000; ++iteration) {
        ControlUiActionGate gate;
        std::atomic_bool began = false;
        std::thread dispatcher([&] { began = gate.TryBegin(); });
        const auto result = gate.CancelOrClassify();
        dispatcher.join();

        if (began.load()) {
            Check(result == ControlUiActionGate::Result::Indeterminate,
                  "a winning dispatcher must make concurrent cancellation indeterminate");
            gate.Complete(true);
        } else {
            Check(result == ControlUiActionGate::Result::Failed, "a winning cancellation must prevent later execution");
        }
    }
}
} // namespace

int RunControlUiActionGateTests() {
    TestCancellationBeforeDispatch();
    TestTimeoutAfterDispatchStarted();
    TestCancellationAndDeadlinePreserveDispatchClassificationForBothUiCommands();
    TestCompletionResults();
    TestBeginCancelRace();
    return g_failures;
}
