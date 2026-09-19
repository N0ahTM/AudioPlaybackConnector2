#include "TestCheck.hpp"

#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>

#include <winrt/Windows.System.Threading.h>

#include <app/PowerTransitionCoordinator.hpp>
#include <app/ResumeReconnectAttemptState.hpp>
#include <app/UiRefreshCoalescer.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <optional>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

namespace {

PowerTransitionCoordinator::CancelTimer UnavailableScheduler(std::chrono::milliseconds,
                                                             PowerTransitionCoordinator::Tick) {
    return {};
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UI Refresh Tests //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestUiRefreshCoalescesFlagsAndDrainsExactlyOnce() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_refresh = 1U << 0;
    constexpr UiRefreshCoalescer::Flags c_forceError = 1U << 1;

    Check(!coalescer.DrainScheduled(), "a new UI refresh coalescer must not have a scheduled drain");
    Check(coalescer.Request(c_refresh), "the first UI refresh request must schedule one drain");
    Check(!coalescer.Request(c_forceError), "a second UI refresh request must not schedule a duplicate drain");
    Check(coalescer.DrainScheduled(), "coalesced UI refresh requests must keep the drain scheduled");

    const auto flags = coalescer.BeginDrain();
    Check(flags == (c_refresh | c_forceError), "a UI drain must atomically consume the union of pending flags");
    Check(coalescer.DrainScheduled(), "a UI drain must remain scheduled until completion is committed");
    Check(!coalescer.CompleteDrain(), "a completed UI drain without new work must stop");
    Check(!coalescer.DrainScheduled(), "a clean UI drain completion must clear its scheduled state");
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
        Check(coalescer.DrainScheduled(), "a raced UI request must always leave a drain scheduled");
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

void TestUiRefreshScheduleFailureAllowsAReplacementSchedule() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_first = 1U << 2;
    constexpr UiRefreshCoalescer::Flags c_replacement = 1U << 3;

    Check(coalescer.Request(c_first), "the schedule-failure test must reserve its initial drain");
    Check(!coalescer.ScheduleFailed(), "a failed queue without a concurrent request must not spin-retry");
    Check(!coalescer.DrainScheduled(), "a failed queue operation must release its drain reservation");
    Check(coalescer.Request(c_replacement), "the next request after a queue failure must schedule a replacement drain");
    Check(coalescer.BeginDrain() == (c_first | c_replacement),
          "a replacement drain must retain flags whose earlier queue operation failed");
    Check(!coalescer.CompleteDrain(), "a replacement drain must finish cleanly after consuming retained flags");
}

void TestUiRefreshScheduleFailureRetainsAConcurrentRequest() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_first = 1U << 4;
    constexpr UiRefreshCoalescer::Flags c_concurrent = 1U << 5;

    Check(coalescer.Request(c_first), "the failure-race test must reserve its first schedule");
    Check(!coalescer.Request(c_concurrent), "a concurrent request must join the outstanding schedule");
    Check(coalescer.ScheduleFailed(), "queue failure must atomically reserve a retry for concurrent work");
    Check(coalescer.DrainScheduled(), "the retry reservation must remain visible to later requests");
    Check(coalescer.BeginDrain() == (c_first | c_concurrent),
          "the replacement schedule must preserve original and concurrent flags");
    Check(!coalescer.CompleteDrain(), "the failure-race replacement drain must finish cleanly");
}

void TestUiRefreshCancellationIsTerminal() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_flag = 1U << 3;

    static_cast<void>(coalescer.Request(c_flag));
    coalescer.Cancel();
    Check(!coalescer.DrainScheduled(), "UI refresh cancellation must clear the scheduled drain");
    Check(coalescer.BeginDrain() == 0, "UI refresh cancellation must discard pending flags");
    Check(!coalescer.CompleteDrain(), "completion after UI refresh cancellation must remain idle");
    static_cast<void>(coalescer.ScheduleFailed());
    Check(!coalescer.Request(c_flag), "a cancelled UI refresh coalescer must reject all later requests");
    Check(!coalescer.DrainScheduled(), "a cancelled UI refresh coalescer must remain terminally idle");
}

void TestUiRefreshAbandonRetainsDirtyFlagsForANewRequest() {
    UiRefreshCoalescer coalescer;
    constexpr UiRefreshCoalescer::Flags c_initial = 1U << 6;
    constexpr UiRefreshCoalescer::Flags c_wakeup = 1U << 7;

    Check(coalescer.Request(c_initial), "the abandoned-schedule test must reserve its initial drain");
    coalescer.AbandonSchedule();
    Check(!coalescer.DrainScheduled(), "abandoning an unavailable scheduler must release its reservation");
    Check(coalescer.Request(c_wakeup), "a later event must wake dirty work after scheduler abandonment");
    Check(coalescer.BeginDrain() == (c_initial | c_wakeup),
          "scheduler abandonment must retain all dirty flags for the later wakeup");
    Check(!coalescer.CompleteDrain(), "the recovered abandoned schedule must drain cleanly");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Power Recovery Tests //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestResumeReconnectCountsOnlyActuallyStartedAttempts() {
    ResumeReconnectAttemptState state;
    state.BeginCycle({L"alpha", L"beta", L"alpha", L""});
    Check(state.Size() == 2, "resume targets must be non-empty and de-duplicated");

    for (unsigned int round = 0; round < 6; ++round) {
        auto selection = state.SelectEligible(6);
        Check(selection.Eligible.size() == 2, "busy or skipped resume targets must remain eligible");
        state.RecordAttempts({L"alpha"});
    }

    auto selection = state.SelectEligible(6);
    Check(selection.Exhausted == std::vector<std::wstring>{L"alpha"},
          "only the target with six real attempts must be exhausted");
    Check(selection.Eligible == std::vector<std::wstring>{L"beta"},
          "a target that was repeatedly skipped must retain its full retry budget");

    for (unsigned int round = 0; round < 8; ++round)
        state.RecordAttempts({});
    selection = state.SelectEligible(6);
    Check(selection.Eligible == std::vector<std::wstring>{L"beta"},
          "empty delivery completions must not consume retry budget");
}

void TestResumeReconnectPreservesPendingTargetsAcrossSuspendCycles() {
    ResumeReconnectAttemptState state;
    state.BeginCycle({L"alpha"});
    state.RecordAttempts({L"alpha", L"alpha"});
    state.BeginCycle({L"beta", L"alpha"});

    auto selection = state.SelectEligible(1);
    Check(selection.Exhausted.empty(), "a new suspend cycle must reset prior retry counts");
    Check(selection.Eligible == std::vector<std::wstring>({L"alpha", L"beta"}),
          "a new suspend cycle must preserve pending targets and merge newly active targets");
    Check(state.Acknowledge(L"alpha"), "a connected resume target must be acknowledged");
    Check(!state.Acknowledge(L"missing"), "acknowledging an unknown target must be a no-op");
    Check(state.Size() == 1, "acknowledgement must remove exactly one pending target");
    state.Clear();
    Check(state.Empty(), "clearing resume state must remove all pending targets");
}

void TestResumeReconnectDeliversOnceWhenSchedulerIsUnavailable() {
    std::atomic_bool exiting = false;
    PowerTransitionCoordinator coordinator(exiting, UnavailableScheduler);
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha", L"beta"}; });

    std::size_t callbackCount = 0;
    std::vector<std::wstring> deliveredIds;
    std::optional<std::uint64_t> generation;
    coordinator.HandleResume(nullptr,
                             [&](std::vector<std::wstring> const& deviceIds,
                                 std::uint64_t callbackGeneration,
                                 PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
                                 ++callbackCount;
                                 deliveredIds = deviceIds;
                                 generation = callbackGeneration;
                                 completed(std::move(deviceIds));
                             });

    Check(callbackCount == 1, "scheduler failure must deliver pending resume recovery once immediately");
    Check(deliveredIds == std::vector<std::wstring>({L"alpha", L"beta"}),
          "immediate resume recovery must retain every pending target");
    Check(generation.has_value() && coordinator.IsResumeReconnectGenerationCurrent(*generation),
          "immediate delivery must retain the active recovery generation until connections acknowledge it");

    coordinator.HandleResume(
        nullptr, [&](std::vector<std::wstring>, std::uint64_t, PowerTransitionCoordinator::ResumeReconnectCompleted) {
            ++callbackCount;
        });
    Check(callbackCount == 1, "an unavailable scheduler must not create a duplicate resume recovery delivery");
}

void TestResumeReconnectFallbackRejectsStaleAndCancelledCompletions() {
    std::atomic_bool exiting = false;
    PowerTransitionCoordinator coordinator(exiting, UnavailableScheduler);
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });

    std::optional<PowerTransitionCoordinator::ResumeReconnectCompleted> staleCompletion;
    std::uint64_t staleGeneration = 0;
    coordinator.HandleResume(nullptr,
                             [&](std::vector<std::wstring>,
                                 std::uint64_t generation,
                                 PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
                                 staleGeneration = generation;
                                 staleCompletion = std::move(completed);
                             });
    Check(staleCompletion.has_value(), "initial immediate delivery must provide a completion callback");

    coordinator.HandleSuspend({}, nullptr);
    Check(!coordinator.IsResumeReconnectGenerationCurrent(staleGeneration),
          "a new suspend cycle must invalidate an earlier fallback generation");
    (*staleCompletion)({L"alpha", L"alpha", L"alpha", L"alpha", L"alpha", L"alpha"});

    std::optional<PowerTransitionCoordinator::ResumeReconnectCompleted> currentCompletion;
    std::size_t callbackCount = 0;
    std::uint64_t currentGeneration = 0;
    coordinator.HandleResume(nullptr,
                             [&](std::vector<std::wstring> const& deviceIds,
                                 std::uint64_t generation,
                                 PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
                                 ++callbackCount;
                                 currentGeneration = generation;
                                 Check(deviceIds == std::vector<std::wstring>{L"alpha"},
                                       "a stale completion must not consume the replacement cycle retry budget");
                                 currentCompletion = std::move(completed);
                             });
    Check(callbackCount == 1, "a replacement generation must receive its own fallback delivery");
    Check(currentCompletion.has_value(), "the replacement fallback delivery must retain its completion callback");

    coordinator.Cancel();
    (*currentCompletion)({L"alpha"});
    Check(!coordinator.IsResumeReconnectGenerationCurrent(currentGeneration),
          "cancellation must keep a late fallback completion from restoring recovery state");
}

void TestResumeReconnectFallbackCompletionOutlivesCoordinatorSafely() {
    std::atomic_bool exiting = false;
    std::optional<PowerTransitionCoordinator::ResumeReconnectCompleted> retainedCompletion;
    {
        PowerTransitionCoordinator coordinator(exiting, UnavailableScheduler);
        coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });
        coordinator.HandleResume(nullptr,
                                 [&](std::vector<std::wstring>,
                                     std::uint64_t,
                                     PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
                                     retainedCompletion = std::move(completed);
                                 });
    }

    Check(retainedCompletion.has_value(), "fallback delivery must retain a completion callback for teardown coverage");
    (*retainedCompletion)({L"alpha"});
    Check(retainedCompletion.has_value(),
          "a retained fallback completion must access only shared recovery state after coordinator teardown");
}

struct ManualResumeTimer {
    PowerTransitionCoordinator::Tick Tick;
    bool Cancelled = false;

    PowerTransitionCoordinator::CancelTimer Schedule(std::chrono::milliseconds period,
                                                     PowerTransitionCoordinator::Tick tick) {
        Check(period == std::chrono::seconds{10}, "resume retry interval must remain ten seconds");
        Tick = std::move(tick);
        Cancelled = false;
        return [this]() noexcept { Cancelled = true; };
    }
};

void TestResumeDeliveryIsSingleFlightAndCompletionIsConsumedOnce() {
    std::atomic_bool exiting = false;
    ManualResumeTimer timer;
    PowerTransitionCoordinator coordinator(
        exiting, [&](auto period, auto tick) { return timer.Schedule(period, std::move(tick)); });
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha", L"alpha", L""}; });
    std::vector<PowerTransitionCoordinator::ResumeReconnectCompleted> completions;
    coordinator.HandleResume({}, [&](auto ids, auto, auto completed) {
        Check(ids == std::vector<std::wstring>{L"alpha"}, "delivery must deduplicate nonempty recovery targets");
        completions.push_back(std::move(completed));
    });
    Check(completions.empty(), "a working scheduler must preserve the reconnect delay");
    Check(timer.Tick(), "first scheduled delivery must remain active");
    Check(timer.Tick() && completions.size() == 1, "ticks must not overlap an outstanding delivery");
    auto first = completions.front();
    first({L"alpha", L"alpha"});
    for (int duplicate = 0; duplicate < 8; ++duplicate)
        first({L"alpha"});
    Check(timer.Tick() && completions.size() == 2, "duplicate completions must not consume retry budget");
    first({L"alpha"});
    Check(timer.Tick() && completions.size() == 2, "an old completion must not finish the next delivery");
    completions.back()({L"alpha"});
    for (int attempt = 2; attempt < 6; ++attempt) {
        Check(timer.Tick(), "each target retains six actual attempt opportunities");
        completions.back()({L"alpha"});
    }
    Check(completions.size() == 6 && !timer.Tick(), "six completed attempts must stop the periodic schedule");
}

void TestResumeCancelDuringDeliveryRejectsLateCompletionAndTicks() {
    std::atomic_bool exiting = false;
    ManualResumeTimer timer;
    PowerTransitionCoordinator coordinator(
        exiting, [&](auto period, auto tick) { return timer.Schedule(period, std::move(tick)); });
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });
    std::barrier admitted{2};
    std::barrier release{2};
    std::uint64_t generation = 0;
    coordinator.HandleResume({}, [&](auto, auto deliveredGeneration, auto completed) {
        generation = deliveredGeneration;
        admitted.arrive_and_wait();
        release.arrive_and_wait();
        completed({L"alpha"});
    });
    std::jthread worker([tick = timer.Tick] { static_cast<void>(tick()); });
    admitted.arrive_and_wait();
    coordinator.Cancel();
    Check(timer.Cancelled, "cancellation must disarm the owned schedule without waiting on foreign delivery");
    release.arrive_and_wait();
    worker.join();
    Check(!coordinator.IsResumeReconnectGenerationCurrent(generation),
          "late completion must not restore a cancelled cycle");
    Check(!timer.Tick(), "a callback admitted before cancellation must reject later deliveries");
}

void TestResumeCallbackCaptureRetiresUnlockedAndTimerOutlivesFacade() {
    std::atomic_bool exiting = false;
    ManualResumeTimer timer;
    bool captureRetired = false;
    {
        PowerTransitionCoordinator coordinator(
            exiting, [&](auto period, auto tick) { return timer.Schedule(period, std::move(tick)); });
        coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });
        auto capture = std::shared_ptr<int>(new int{0}, [&](int* value) {
            captureRetired = true;
            Check(!coordinator.IsResumeReconnectGenerationCurrent(1),
                  "retired callback may reenter generation inspection");
            delete value;
        });
        coordinator.HandleResume({}, [capture = std::move(capture)](auto, auto, auto) {});
        coordinator.Cancel();
        Check(captureRetired, "cancellation must release foreign captures outside the state lock");
    }
    Check(!timer.Tick(), "retained timer callback must safely reject delivery after facade destruction");
}

void TestSuspendCallbackCannotRestoreCancelledRecovery() {
    std::atomic_bool exiting = false;
    PowerTransitionCoordinator coordinator(exiting, UnavailableScheduler);
    coordinator.HandleSuspend([&] { coordinator.Cancel(); }, [] { return std::vector<std::wstring>{L"alpha"}; });
    bool delivered = false;
    coordinator.HandleResume({}, [&](auto, auto, auto) { delivered = true; });
    Check(!delivered, "cancellation reentered during suspend must fence the returned recovery targets");
}

void TestNativeResumeTimerCancelsWhileDeliveryIsBlocked() {
    struct DeliveryProbe {
        std::binary_semaphore Entered{0};
        std::binary_semaphore Release{0};
        std::binary_semaphore Finished{0};
        std::uint64_t Generation = 0;
    };
    auto probe = std::make_shared<DeliveryProbe>();
    std::atomic_bool exiting = false;
    PowerTransitionCoordinator coordinator(exiting);
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });
    coordinator.HandleResume({}, [probe](auto ids, auto generation, auto completed) {
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)),
              "native resume delivery must initialize its Windows Runtime apartment");
        probe->Generation = generation;
        probe->Entered.release();
        probe->Release.acquire();
        completed(std::move(ids));
        probe->Finished.release();
    });
    bool const entered = probe->Entered.try_acquire_for(std::chrono::seconds{20});
    Check(entered, "native resume timer must deliver after its ten-second delay");
    coordinator.Cancel();
    probe->Release.release();
    if (entered) {
        Check(probe->Finished.try_acquire_for(std::chrono::seconds{5}),
              "admitted native delivery must finish safely after timer cancellation");
        Check(!coordinator.IsResumeReconnectGenerationCurrent(probe->Generation),
              "native late completion must not revive cancelled recovery");
    }
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Test Entry Point //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunAppWorkCoordinatorTests() {
    TestUiRefreshCoalescesFlagsAndDrainsExactlyOnce();
    TestUiRefreshRetainsRequestsMadeDuringDrain();
    TestUiRefreshRequestCompletionRaceHasNoLostWakeup();
    TestUiRefreshConcurrentRequestsUnionFlagsAndScheduleOnce();
    TestUiRefreshScheduleFailureAllowsAReplacementSchedule();
    TestUiRefreshScheduleFailureRetainsAConcurrentRequest();
    TestUiRefreshCancellationIsTerminal();
    TestUiRefreshAbandonRetainsDirtyFlagsForANewRequest();
    TestResumeReconnectCountsOnlyActuallyStartedAttempts();
    TestResumeReconnectPreservesPendingTargetsAcrossSuspendCycles();
    TestResumeReconnectDeliversOnceWhenSchedulerIsUnavailable();
    TestResumeReconnectFallbackRejectsStaleAndCancelledCompletions();
    TestResumeReconnectFallbackCompletionOutlivesCoordinatorSafely();
    TestResumeDeliveryIsSingleFlightAndCompletionIsConsumedOnce();
    TestResumeCancelDuringDeliveryRejectsLateCompletionAndTicks();
    TestResumeCallbackCaptureRetiresUnlockedAndTimerOutlivesFacade();
    TestSuspendCallbackCannotRestoreCancelledRecovery();
    TestNativeResumeTimerCancelsWhileDeliveryIsBlocked();
    return g_failures;
}
