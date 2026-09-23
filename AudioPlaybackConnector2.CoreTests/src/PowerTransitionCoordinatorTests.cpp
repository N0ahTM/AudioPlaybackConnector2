#include "TestCheck.hpp"

#include <windows.h>
#include <objbase.h>
#include <app/PowerTransitionCoordinator.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// PowerTransitionCoordinator Tests //////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

PowerTransitionCoordinator::CancelTimer UnavailableScheduler(std::chrono::milliseconds,
                                                             PowerTransitionCoordinator::Tick) {
    return {};
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

void TestResumeCountsOnlyStartedTargets() {
    std::atomic_bool exiting = false;
    ManualResumeTimer timer;
    PowerTransitionCoordinator coordinator(
        exiting, [&](auto period, auto tick) { return timer.Schedule(period, std::move(tick)); });
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha", L"beta", L"alpha", L""}; });
    std::vector<std::wstring> delivered;
    std::vector<std::wstring> attempted{L"alpha", L"alpha"};
    std::uint64_t generation = 0;
    coordinator.HandleResume({}, [&](auto ids, auto current, auto completed) {
        delivered = std::move(ids);
        generation = current;
        completed(attempted);
    });
    for (int count = 0; count < 6; ++count) {
        Check(timer.Tick() && delivered == std::vector<std::wstring>({L"alpha", L"beta"}),
              "only actual attempts count, and duplicate completion IDs count once");
    }
    attempted.clear();
    for (int skipped = 0; skipped < 8; ++skipped) {
        Check(timer.Tick() && delivered == std::vector<std::wstring>{L"beta"},
              "exhausted targets leave delivery while skipped targets retain their retry budget");
    }
    coordinator.NotifyDeviceConnected(L"unknown");
    Check(!timer.Cancelled && coordinator.IsResumeReconnectGenerationCurrent(generation),
          "an unrelated connection must not acknowledge a pending recovery target");
    coordinator.NotifyDeviceConnected(L"beta");
    Check(timer.Cancelled && !timer.Tick() && !coordinator.IsResumeReconnectGenerationCurrent(generation),
          "acknowledging the final target must stop recovery and reject late ticks");
}

void TestResumeRetainsTargetsAndResetsBudgetAcrossSuspend() {
    std::atomic_bool exiting = false;
    ManualResumeTimer timer;
    PowerTransitionCoordinator coordinator(
        exiting, [&](auto period, auto tick) { return timer.Schedule(period, std::move(tick)); });
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"alpha"}; });
    auto completedAttempts = [](auto ids, auto, auto completed) { completed(std::move(ids)); };
    coordinator.HandleResume({}, completedAttempts);
    for (int count = 0; count < 5; ++count)
        Check(timer.Tick(), "initial cycle must retain unfinished recovery");
    auto staleTick = timer.Tick;
    coordinator.HandleSuspend({}, [] { return std::vector<std::wstring>{L"beta", L"alpha"}; });
    Check(timer.Cancelled && !staleTick(), "suspend must invalidate the prior recovery generation");
    std::vector<std::wstring> delivered;
    coordinator.HandleResume({}, [&](auto ids, auto, auto completed) {
        delivered = ids;
        completed(std::move(ids));
    });
    for (int count = 0; count < 6; ++count) {
        Check(timer.Tick() && delivered == std::vector<std::wstring>({L"alpha", L"beta"}),
              "a new suspend cycle must retain pending targets, merge new targets and reset retry budgets");
    }
    Check(!timer.Tick(), "both targets must finish after six actual attempts in the new cycle");
    coordinator.Cancel();
    Check(timer.Cancelled && !timer.Tick(), "terminal cancellation must keep recovery inactive");
}

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

int RunPowerTransitionCoordinatorTests() {
    TestResumeReconnectDeliversOnceWhenSchedulerIsUnavailable();
    TestResumeReconnectFallbackRejectsStaleAndCancelledCompletions();
    TestResumeReconnectFallbackCompletionOutlivesCoordinatorSafely();
    TestResumeCountsOnlyStartedTargets();
    TestResumeRetainsTargetsAndResetsBudgetAcrossSuspend();
    TestResumeDeliveryIsSingleFlightAndCompletionIsConsumedOnce();
    TestResumeCancelDuringDeliveryRejectsLateCompletionAndTicks();
    TestResumeCallbackCaptureRetiresUnlockedAndTimerOutlivesFacade();
    TestSuspendCallbackCannotRestoreCancelledRecovery();
    TestNativeResumeTimerCancelsWhileDeliveryIsBlocked();
    return g_failures;
}
