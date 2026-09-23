#include "TestCheck.hpp"

#include <app/AdaptiveScheduleState.hpp>

#include <chrono>

namespace {
using namespace std::chrono_literals;

AdaptiveScheduleState::TimePoint At(std::chrono::seconds elapsed) {
    return AdaptiveScheduleState::TimePoint{} + elapsed;
}

void TestSupersededAndEarlyCallbacks() {
    AdaptiveScheduleState schedule;
    auto const first = schedule.Supersede();
    Check(schedule.SetWin32NotBefore(first, At(10s)), "the current schedule may arm its Win32 deadline");
    Check(!schedule.ConsumeWin32IfDue(At(9s)), "an early or stale WM_TIMER must not consume the active deadline");
    Check(schedule.ConsumeWin32IfDue(At(10s)), "the active Win32 deadline must be consumable exactly once");
    Check(!schedule.ConsumeWin32IfDue(At(11s)), "a duplicate WM_TIMER must be ignored after consumption");

    auto const superseded = schedule.Supersede();
    auto const current = schedule.Supersede();
    Check(current != superseded, "each reschedule must receive a distinct current generation");
    Check(!schedule.Consume(superseded), "a queued dispatcher tick from an older schedule must be ignored");
    Check(schedule.Consume(current), "the current dispatcher tick must remain live");
    Check(!schedule.Consume(current), "a duplicate dispatcher tick must be rejected after consumption");

    auto const cancelled = schedule.Supersede();
    static_cast<void>(schedule.Supersede());
    Check(!schedule.Consume(cancelled), "cancelling a schedule must invalidate already queued callbacks");
}
} // namespace

int RunAdaptiveScheduleStateTests() {
    TestSupersededAndEarlyCallbacks();
    return g_failures;
}
