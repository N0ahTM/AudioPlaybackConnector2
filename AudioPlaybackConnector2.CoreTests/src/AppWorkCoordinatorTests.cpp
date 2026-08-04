#include <app/DeferredSaveCoordinator.hpp>
#include <app/ResumeReconnectAttemptState.hpp>
#include <app/UiRefreshCoalescer.hpp>
#include <core/Settings.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestDeferredSaveCoalescesDirtyGenerations() {
    DeferredSaveCoordinator coordinator;
    Check(coordinator.Generation() == 0, "a new deferred-save coordinator must start at generation zero");
    Check(!coordinator.WorkerActive(), "a new deferred-save coordinator must not report an active worker");

    auto first = coordinator.MarkDirty();
    Check(first.Generation == 1, "the first dirty request must advance the generation");
    Check(first.WorkerToStart.has_value(), "the first dirty request must start one worker");
    Check(coordinator.WorkerActive(), "starting a worker must make the worker observable as active");

    auto second = coordinator.MarkDirty();
    Check(second.Generation == 2, "a coalesced dirty request must still advance the generation");
    Check(!second.WorkerToStart, "a coalesced dirty request must not start a duplicate worker");

    const auto worker = *first.WorkerToStart;
    auto wrongWorker = worker;
    ++wrongWorker.Value;
    Check(!coordinator.BeginAttempt(wrongWorker), "a stale worker token must not begin a save attempt");

    auto attempt = coordinator.BeginAttempt(worker);
    Check(attempt.has_value(), "the active worker must be able to begin its save attempt");
    Check(attempt && attempt->Generation == second.Generation,
          "an attempt must capture the newest coalesced generation");
    Check(!coordinator.BeginAttempt(worker), "one worker must never run two concurrent save attempts");

    if (attempt) {
        auto wrongAttempt = *attempt;
        ++wrongAttempt.Generation;
        Check(coordinator.CompleteAttempt(wrongAttempt, true) == DeferredSaveCoordinator::Completion::Stale,
              "a completion for the wrong generation must be rejected without consuming the real attempt");
        Check(coordinator.CompleteAttempt(*attempt, true) == DeferredSaveCoordinator::Completion::Stop,
              "a successful attempt for the newest generation must stop the worker");
        Check(coordinator.CompleteAttempt(*attempt, true) == DeferredSaveCoordinator::Completion::Stale,
              "a duplicate completion must be stale");
    }
    Check(!coordinator.WorkerActive(), "a clean successful save must leave no active worker");
}

void TestDeferredSaveRetainsMutationsDuringAnAttempt() {
    DeferredSaveCoordinator coordinator;
    auto initial = coordinator.MarkDirty();
    const auto worker = *initial.WorkerToStart;
    auto firstAttempt = coordinator.BeginAttempt(worker);
    Check(firstAttempt.has_value(), "the initial deferred-save attempt must start");

    auto concurrentMutation = coordinator.MarkDirty();
    Check(!concurrentMutation.WorkerToStart,
          "a mutation during an attempt must remain assigned to the existing worker");
    if (firstAttempt) {
        Check(coordinator.CompleteAttempt(*firstAttempt, true) ==
                  DeferredSaveCoordinator::Completion::ContinueAfterDebounce,
              "a mutation during an attempt must force a follow-up save");
    }
    Check(coordinator.WorkerActive(), "the worker must remain active while a newer generation is dirty");

    auto secondAttempt = coordinator.BeginAttempt(worker);
    Check(secondAttempt && secondAttempt->Generation == concurrentMutation.Generation,
          "the follow-up attempt must capture the generation created during the first attempt");
    if (secondAttempt) {
        Check(coordinator.CompleteAttempt(*secondAttempt, true) == DeferredSaveCoordinator::Completion::Stop,
              "the follow-up attempt must stop after persisting all changes");
    }
}

void TestDeferredSaveRetriesFailuresWithoutLosingDirtyState() {
    DeferredSaveCoordinator coordinator;
    auto request = coordinator.MarkDirty();
    const auto worker = *request.WorkerToStart;
    auto failedAttempt = coordinator.BeginAttempt(worker);
    Check(failedAttempt.has_value(), "the failed-save test must begin an attempt");

    if (failedAttempt) {
        Check(coordinator.CompleteAttempt(*failedAttempt, false) ==
                  DeferredSaveCoordinator::Completion::RetryWithBackoff,
              "a failed save must request a bounded retry");
        Check(coordinator.CompleteAttempt(*failedAttempt, true) == DeferredSaveCoordinator::Completion::Stale,
              "a failed attempt must be consumed exactly once");
    }
    Check(coordinator.WorkerActive(), "a failed save must retain its worker and dirty state");

    auto retry = coordinator.BeginAttempt(worker);
    Check(retry && retry->Generation == request.Generation,
          "a retry without a new mutation must retry the same generation");
    if (retry) {
        Check(coordinator.CompleteAttempt(*retry, true) == DeferredSaveCoordinator::Completion::Stop,
              "a successful retry must cleanly stop the worker");
    }
}

void TestDeferredSaveCancellationInvalidatesOutstandingWork() {
    DeferredSaveCoordinator coordinator;
    auto request = coordinator.MarkDirty();
    const auto worker = *request.WorkerToStart;
    auto attempt = coordinator.BeginAttempt(worker);
    Check(attempt.has_value(), "the cancellation test must begin an attempt");

    coordinator.Cancel();
    Check(!coordinator.WorkerActive(), "cancellation must clear the active worker");
    if (attempt) {
        Check(coordinator.CompleteAttempt(*attempt, true) == DeferredSaveCoordinator::Completion::Stale,
              "completion after cancellation must be stale");
    }
    Check(!coordinator.BeginAttempt(worker), "a cancelled worker must not begin another attempt");

    const auto generationBeforeRejectedRequest = coordinator.Generation();
    auto rejected = coordinator.MarkDirty();
    Check(rejected.Generation == generationBeforeRejectedRequest && !rejected.WorkerToStart,
          "a stopped coordinator must reject new dirty work without changing generations");
    coordinator.Cancel();
    Check(!coordinator.WorkerActive(), "cancellation must be idempotent");
}

void TestDeferredSaveCanRecoverFromSchedulerFailure() {
    DeferredSaveCoordinator coordinator;
    auto request = coordinator.MarkDirty();
    const auto worker = *request.WorkerToStart;
    auto attempt = coordinator.BeginAttempt(worker);
    Check(attempt.has_value(), "the scheduler-failure test must begin an attempt");
    Check(coordinator.AbandonWorker(worker), "the current worker must be abandonable after scheduling failure");
    Check(!coordinator.WorkerActive(), "abandoning an unschedulable worker must release ownership");
    if (attempt) {
        Check(coordinator.CompleteAttempt(*attempt, true) == DeferredSaveCoordinator::Completion::Stale,
              "an abandoned worker completion must be stale");
    }

    auto replacement = coordinator.MarkDirty();
    Check(replacement.WorkerToStart.has_value(), "a later mutation must be able to start a replacement worker");
    Check(replacement.WorkerToStart != request.WorkerToStart,
          "a replacement worker must use a distinct token from the abandoned worker");
}

void TestDeferredSavePersistsMutationMadeAfterSnapshot() {
    DeferredSaveCoordinator coordinator;
    std::atomic<int> liveValue{1};
    std::atomic<int> persistedValue{0};
    std::barrier snapshotCaptured{2};
    std::barrier mutationRecorded{2};

    auto request = coordinator.MarkDirty();
    const auto worker = *request.WorkerToStart;
    DeferredSaveCoordinator::Completion firstCompletion = DeferredSaveCoordinator::Completion::Stale;
    DeferredSaveCoordinator::Completion secondCompletion = DeferredSaveCoordinator::Completion::Stale;
    bool firstAttemptStarted = false;
    bool secondAttemptStarted = false;
    bool mutationStartedDuplicateWorker = true;

    std::jthread saveThread([&]() {
        auto firstAttempt = coordinator.BeginAttempt(worker);
        firstAttemptStarted = firstAttempt.has_value();
        const int firstSnapshot = liveValue.load();
        snapshotCaptured.arrive_and_wait();
        mutationRecorded.arrive_and_wait();

        persistedValue.store(firstSnapshot);
        if (!firstAttempt) return;
        firstCompletion = coordinator.CompleteAttempt(*firstAttempt, true);
        if (firstCompletion != DeferredSaveCoordinator::Completion::ContinueAfterDebounce) return;

        auto secondAttempt = coordinator.BeginAttempt(worker);
        secondAttemptStarted = secondAttempt.has_value();
        if (!secondAttempt) return;
        persistedValue.store(liveValue.load());
        secondCompletion = coordinator.CompleteAttempt(*secondAttempt, true);
    });
    std::jthread mutationThread([&]() {
        snapshotCaptured.arrive_and_wait();
        liveValue.store(2);
        auto mutation = coordinator.MarkDirty();
        mutationStartedDuplicateWorker = mutation.WorkerToStart.has_value();
        mutationRecorded.arrive_and_wait();
    });
    saveThread.join();
    mutationThread.join();

    Check(firstAttemptStarted, "the deterministic snapshot race must begin its first attempt");
    Check(!mutationStartedDuplicateWorker, "a post-snapshot mutation must reuse the active worker");
    Check(firstCompletion == DeferredSaveCoordinator::Completion::ContinueAfterDebounce,
          "a post-snapshot mutation must not be acknowledged by the older save");
    Check(secondAttemptStarted, "a post-snapshot mutation must receive a second attempt");
    Check(secondCompletion == DeferredSaveCoordinator::Completion::Stop,
          "the second race attempt must stop after persisting the new value");
    Check(persistedValue.load() == 2, "the deterministic snapshot race must persist the newest value");
}

void TestDeferredSaveRequestCompletionRaceHasNoLostWakeup() {
    constexpr std::size_t c_iterations = 128;
    for (std::size_t iteration = 0; iteration < c_iterations; ++iteration) {
        DeferredSaveCoordinator coordinator;
        auto initial = coordinator.MarkDirty();
        const auto initialWorker = *initial.WorkerToStart;
        auto attempt = coordinator.BeginAttempt(initialWorker);
        Check(attempt.has_value(), "each request/completion race must begin an attempt");
        if (!attempt) continue;

        std::barrier raceStart{2};
        DeferredSaveCoordinator::Completion completion = DeferredSaveCoordinator::Completion::Stale;
        DeferredSaveCoordinator::RequestResult request;
        std::jthread completionThread([&]() {
            raceStart.arrive_and_wait();
            completion = coordinator.CompleteAttempt(*attempt, true);
        });
        std::jthread requestThread([&]() {
            raceStart.arrive_and_wait();
            request = coordinator.MarkDirty();
        });
        completionThread.join();
        requestThread.join();

        const bool requestWon =
            completion == DeferredSaveCoordinator::Completion::ContinueAfterDebounce && !request.WorkerToStart;
        const bool completionWon =
            completion == DeferredSaveCoordinator::Completion::Stop && request.WorkerToStart.has_value();
        Check(requestWon || completionWon,
              "a request racing a completion must either continue the old worker or start exactly one new worker");
        Check(coordinator.WorkerActive(), "a raced dirty request must always leave a worker active");

        const auto currentWorker = request.WorkerToStart.value_or(initialWorker);
        auto finalAttempt = coordinator.BeginAttempt(currentWorker);
        Check(finalAttempt && finalAttempt->Generation == request.Generation,
              "the surviving worker must own the raced dirty generation");
        if (finalAttempt) {
            Check(coordinator.CompleteAttempt(*finalAttempt, true) == DeferredSaveCoordinator::Completion::Stop,
                  "the surviving worker must be able to finish the raced request");
        }
    }
}

void TestDeferredSaveConcurrentRequestsStartExactlyOneWorker() {
    constexpr std::size_t c_threadCount = 32;
    DeferredSaveCoordinator coordinator;
    std::barrier start{c_threadCount};
    std::array<DeferredSaveCoordinator::RequestResult, c_threadCount> results{};
    std::vector<std::jthread> threads;
    threads.reserve(c_threadCount);
    for (std::size_t index = 0; index < c_threadCount; ++index) {
        threads.emplace_back([&, index]() {
            start.arrive_and_wait();
            results[index] = coordinator.MarkDirty();
        });
    }
    threads.clear();

    std::size_t workersStarted = 0;
    std::optional<DeferredSaveCoordinator::WorkerToken> worker;
    std::vector<std::uint64_t> generations;
    generations.reserve(c_threadCount);
    for (auto const& result : results) {
        generations.push_back(result.Generation);
        if (result.WorkerToStart) {
            ++workersStarted;
            worker = result.WorkerToStart;
        }
    }
    std::ranges::sort(generations);
    bool generationsAreComplete = generations.size() == c_threadCount;
    for (std::size_t index = 0; index < generations.size(); ++index) {
        generationsAreComplete = generationsAreComplete && generations[index] == index + 1;
    }

    Check(workersStarted == 1, "concurrent dirty requests must start exactly one worker");
    Check(generationsAreComplete, "concurrent dirty requests must receive distinct serialized generations");
    Check(coordinator.Generation() == c_threadCount, "all concurrent dirty requests must be recorded");
    if (worker) {
        auto attempt = coordinator.BeginAttempt(*worker);
        Check(attempt && attempt->Generation == c_threadCount,
              "the single concurrent-request worker must capture the newest generation");
        if (attempt) {
            Check(coordinator.CompleteAttempt(*attempt, true) == DeferredSaveCoordinator::Completion::Stop,
                  "the single concurrent-request worker must finish all coalesced work");
        }
    }
}

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

void TestDeferredSaveExternalFlushAcknowledgesOnlyItsGeneration() {
    DeferredSaveCoordinator coordinator;
    auto initial = coordinator.MarkDirty();
    Check(initial.WorkerToStart.has_value(), "external-flush test must start a worker");

    auto staleToken = coordinator.BeginExternalSave();
    static_cast<void>(coordinator.MarkDirty());
    Check(!coordinator.CompleteExternalSave(staleToken, true),
          "an external flush must not acknowledge a mutation from a newer generation");
    Check(coordinator.WorkerActive(), "a stale external flush must leave the active worker intact");

    auto currentToken = coordinator.BeginExternalSave();
    Check(coordinator.CompleteExternalSave(currentToken, true),
          "a clean external flush must acknowledge its current generation");
    Check(!coordinator.WorkerActive(), "an acknowledged external flush must retire the old worker");
    Check(!coordinator.CompleteExternalSave(currentToken, false), "a failed external flush must never be acknowledged");
}

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

void TestSettingsRevisionTracksOnlyCommittedMutations() {
    Settings settings;
    Check(!settings.HasUnsavedChanges(), "new settings must start clean");
    {
        auto locked = settings.LockExclusiveData();
        static_cast<void>(locked->ShowNotifications);
    }
    Check(!settings.HasUnsavedChanges(), "a no-op exclusive settings access must remain clean");
    {
        auto locked = settings.LockExclusiveData();
        locked.MarkChanged();
        locked->ShowNotifications = !locked->ShowNotifications;
    }
    Check(settings.HasUnsavedChanges(), "an actual settings mutation must advance the revision");
}

} // namespace

int RunAppWorkCoordinatorTests() {
    TestDeferredSaveCoalescesDirtyGenerations();
    TestDeferredSaveRetainsMutationsDuringAnAttempt();
    TestDeferredSaveRetriesFailuresWithoutLosingDirtyState();
    TestDeferredSaveCancellationInvalidatesOutstandingWork();
    TestDeferredSaveCanRecoverFromSchedulerFailure();
    TestDeferredSavePersistsMutationMadeAfterSnapshot();
    TestDeferredSaveRequestCompletionRaceHasNoLostWakeup();
    TestDeferredSaveConcurrentRequestsStartExactlyOneWorker();
    TestDeferredSaveExternalFlushAcknowledgesOnlyItsGeneration();
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
    TestSettingsRevisionTracksOnlyCommittedMutations();
    return g_failures;
}

#if defined(APC_APP_WORK_COORDINATOR_TEST_STANDALONE)
int main() {
    return RunAppWorkCoordinatorTests();
}
#endif
