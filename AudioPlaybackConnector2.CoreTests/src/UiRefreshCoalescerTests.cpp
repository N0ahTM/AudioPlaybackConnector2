#include "TestCheck.hpp"

#include <app/UiRefreshCoalescer.hpp>
#include <algorithm>
#include <array>
#include <barrier>
#include <thread>
#include <vector>

namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UiRefreshCoalescer Tests //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestUiRefreshCoalescesFlagsAndDrainsExactlyOnce() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_refresh = 1U << 0;
    constexpr UiRefreshCoalescer::Flags c_forceError = 1U << 1;

    Check(coalescer.Request(c_refresh), "the first UI refresh request must schedule one drain");
    Check(!coalescer.Request(c_forceError), "a second UI refresh request must not schedule a duplicate drain");

    const auto flags = coalescer.BeginDrain();
    Check(flags == (c_refresh | c_forceError), "a UI drain must atomically consume the union of pending flags");
    Check(!coalescer.CompleteDrain(), "a completed UI drain without new work must stop");
}

void TestUiRefreshRetainsRequestsMadeDuringDrain() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_first = 1U << 0;
    constexpr UiRefreshCoalescer::Flags c_duringDrain = 1U << 1;

    Check(coalescer.Request(c_first), "the first staged UI request must schedule a drain");
    Check(coalescer.BeginDrain() == c_first, "the first staged UI drain must consume only its initial flags");
    Check(!coalescer.Request(c_duringDrain), "a request during a drain must reuse the scheduled drain chain");
    Check(coalescer.CompleteDrain(), "work arriving during a drain must request another drain pass");
    Check(coalescer.BeginDrain() == c_duringDrain, "the next drain pass must receive the flags that arrived mid-drain");
    Check(!coalescer.CompleteDrain(), "the final staged UI drain must stop when no flags remain");
}

void TestUiRefreshRequestCompletionRaceHasNoLostWakeup() {
    constexpr std::size_t c_iterations = 128;
    constexpr UiRefreshCoalescer::Flags c_initial = 1U << 0;
    constexpr UiRefreshCoalescer::Flags c_raced = 1U << 1;

    for (std::size_t iteration = 0; iteration < c_iterations; ++iteration) {
        UiRefreshCoalescer coalescer;
        static_cast<void>(coalescer.Request(c_initial));
        Check(coalescer.BeginDrain() == c_initial, "each UI request/completion race must begin from an empty drain");

        std::barrier raceStart{2};
        bool requestScheduledNewDrain = false;
        bool completionContinuedDrain = false;
        std::jthread completionThread([&]() {
            raceStart.arrive_and_wait();
            completionContinuedDrain = coalescer.CompleteDrain();
        });
        std::jthread requestThread([&]() {
            raceStart.arrive_and_wait();
            requestScheduledNewDrain = coalescer.Request(c_raced);
        });
        completionThread.join();
        requestThread.join();

        const bool requestWon = !requestScheduledNewDrain && completionContinuedDrain;
        const bool completionWon = requestScheduledNewDrain && !completionContinuedDrain;
        Check(requestWon || completionWon,
              "a UI request racing completion must continue the old drain or schedule exactly one new drain");
        Check(coalescer.BeginDrain() == c_raced, "a raced UI request must never lose its flags");
        Check(!coalescer.CompleteDrain(), "the drain for raced UI flags must finish cleanly");
    }
}

void TestUiRefreshConcurrentRequestsUnionFlagsAndScheduleOnce() {
    constexpr std::size_t c_threadCount = 16;
    UiRefreshCoalescer coalescer;
    std::barrier start{c_threadCount};
    std::array<bool, c_threadCount> scheduled{};
    std::vector<std::jthread> threads;
    threads.reserve(c_threadCount);
    for (std::size_t index = 0; index < c_threadCount; ++index) {
        threads.emplace_back([&, index]() {
            start.arrive_and_wait();
            scheduled[index] = coalescer.Request(static_cast<UiRefreshCoalescer::Flags>(1U << index));
        });
    }
    threads.clear();

    const auto schedules = static_cast<std::size_t>(std::ranges::count(scheduled, true));
    constexpr UiRefreshCoalescer::Flags c_allFlags = (1U << c_threadCount) - 1U;
    Check(schedules == 1, "concurrent UI refresh requests must schedule exactly one drain");
    Check(coalescer.BeginDrain() == c_allFlags, "concurrent UI refresh requests must preserve every requested flag");
    Check(!coalescer.CompleteDrain(), "the concurrent UI refresh drain must finish after consuming all flags");
}

void TestUiRefreshCancellationIsTerminal() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_flag = 1U << 3;

    static_cast<void>(coalescer.Request(c_flag));
    coalescer.Cancel();
    Check(coalescer.BeginDrain() == 0, "UI refresh cancellation must discard pending flags");
    Check(!coalescer.CompleteDrain(), "completion after UI refresh cancellation must remain idle");
    Check(!coalescer.Request(c_flag), "a cancelled UI refresh coalescer must reject all later requests");
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Test Entry Point //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunUiRefreshCoalescerTests() {
    TestUiRefreshCoalescesFlagsAndDrainsExactlyOnce();
    TestUiRefreshRetainsRequestsMadeDuringDrain();
    TestUiRefreshRequestCompletionRaceHasNoLostWakeup();
    TestUiRefreshConcurrentRequestsUnionFlagsAndScheduleOnce();
    TestUiRefreshCancellationIsTerminal();
    return g_failures;
}
