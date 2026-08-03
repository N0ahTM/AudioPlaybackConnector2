#include <app/ControlUiActionGate.hpp>

#include <atomic>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
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
    TestCompletionResults();
    TestBeginCancelRace();
    return g_failures;
}
