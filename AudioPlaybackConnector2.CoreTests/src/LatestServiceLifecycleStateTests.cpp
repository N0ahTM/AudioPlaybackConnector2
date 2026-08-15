#include <core/LatestServiceLifecycleState.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestStartCompletesToIdle() {
    LatestServiceLifecycleState state;
    auto request = state.RequestStart();
    Check(request.Accepted && request.OperationToStart.has_value(), "an idle start request must elect an executor");
    Check(state.BeginExecution(*request.OperationToStart), "the elected start token must begin execution");
    Check(!state.Complete(*request.OperationToStart), "an unsuperseded start must complete to idle");
    Check(!state.InFlight(), "completed lifecycle work must release the in-flight slot");
}

void TestReentrantStopIsAppliedByCurrentExecutor() {
    LatestServiceLifecycleState state;
    auto start = state.RequestStart();
    Check(state.BeginExecution(*start.OperationToStart), "start must own the executor before reentrant stop");

    auto stop = state.RequestStop();
    Check(stop.Accepted && !stop.OperationToStart,
          "reentrant stop must update desired state without a second executor");
    state.WaitForIdle();

    auto next = state.Complete(*start.OperationToStart);
    Check(next && next->State == LatestServiceLifecycleState::DesiredState::Stopped,
          "the active executor must receive the reentrant stop as follow-up work");
    Check(!next->ClearState, "a normal reentrant stop must preserve cached service state");
    Check(!state.Complete(*next), "the follow-up stop must settle the lifecycle");
}

void TestConcurrentPermanentStopWaitsForSettlement() {
    LatestServiceLifecycleState state;
    auto start = state.RequestStart();
    Check(state.BeginExecution(*start.OperationToStart), "start must begin before the concurrent stop");

    std::atomic_bool stopRequested = false;
    std::atomic_bool stopHadNoExecutor = false;
    std::atomic_bool waiterReturned = false;
    std::jthread waiter([&] {
        auto stop = state.RequestStop(true);
        stopHadNoExecutor.store(!stop.OperationToStart);
        stopRequested.store(true);
        state.WaitForIdle();
        waiterReturned.store(true);
    });

    while (!stopRequested.load())
        std::this_thread::yield();
    Check(stopHadNoExecutor.load(), "a concurrent stop must not create a second executor");
    std::this_thread::sleep_for(10ms);
    Check(!waiterReturned.load(), "a non-reentrant stop must wait while start is still executing");

    auto next = state.Complete(*start.OperationToStart);
    Check(next && next->State == LatestServiceLifecycleState::DesiredState::Stopped && next->ClearState,
          "permanent stop must supersede start and request state cleanup");
    Check(!state.Complete(*next), "permanent stop completion must settle waiters");
    waiter.join();
    Check(waiterReturned.load(), "waiter must return after permanent stop settles");
    Check(state.PermanentlyStopped(), "permanent stop must remain observable");
    Check(!state.RequestStart().Accepted, "start must be rejected after permanent stop");
}

void TestDuplicateRequestsCoalesceOnlyWhileInFlight() {
    LatestServiceLifecycleState state;
    auto first = state.RequestStart();
    auto duplicate = state.RequestStart();
    Check(duplicate.Accepted && duplicate.Coalesced && !duplicate.OperationToStart,
          "duplicate in-flight start must coalesce");
    Check(state.BeginExecution(*first.OperationToStart), "the first start must retain executor ownership");
    Check(!state.Complete(*first.OperationToStart), "the coalesced start must complete once");

    auto retry = state.RequestStart();
    Check(retry.Accepted && !retry.Coalesced && retry.OperationToStart.has_value(),
          "a later start call must be able to retry after a failed service start");
    Check(retry.OperationToStart->Revision != first.OperationToStart->Revision,
          "retry work must use a new lifecycle revision");
}

void TestStaleCompletionCannotReleaseNewerWork() {
    LatestServiceLifecycleState state;
    auto start = state.RequestStart();
    Check(state.BeginExecution(*start.OperationToStart), "start must begin before superseding work");
    static_cast<void>(state.RequestStop());
    auto stop = state.Complete(*start.OperationToStart);
    Check(stop.has_value(), "stop must supersede the completed start");
    Check(!state.Complete(*start.OperationToStart), "stale start completion must be ignored");
    Check(state.InFlight() == stop, "stale completion must leave newer stop work active");
    Check(!state.Complete(*stop), "current stop completion must settle normally");
}

void TestConcurrentRequestStormUsesSingleExecutor() {
    LatestServiceLifecycleState state;
    auto initial = state.RequestStart();
    Check(state.BeginExecution(*initial.OperationToStart), "initial work must own the storm executor");

    constexpr std::size_t threadCount = 8;
    constexpr std::size_t requestsPerThread = 250;
    std::atomic_bool secondExecutorObserved = false;
    std::array<std::jthread, threadCount> requesters;
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        requesters[threadIndex] = std::jthread([&, threadIndex] {
            for (std::size_t requestIndex = 0; requestIndex < requestsPerThread; ++requestIndex) {
                auto request = ((threadIndex + requestIndex) % 2 == 0) ? state.RequestStart() : state.RequestStop();
                if (request.OperationToStart) secondExecutorObserved.store(true);
            }
        });
    }
    for (auto& requester : requesters)
        requester.join();

    Check(!secondExecutorObserved.load(), "concurrent requests must not elect a second lifecycle executor");
    auto final = state.Complete(*initial.OperationToStart);
    if (final) {
        Check(!state.Complete(*final), "the executor must collapse the request storm to one latest follow-up");
    }
    Check(!state.InFlight(), "request storm completion must leave lifecycle state idle");
}

} // namespace

int RunLatestServiceLifecycleStateTests() {
    TestStartCompletesToIdle();
    TestReentrantStopIsAppliedByCurrentExecutor();
    TestConcurrentPermanentStopWaitsForSettlement();
    TestDuplicateRequestsCoalesceOnlyWhileInFlight();
    TestStaleCompletionCannotReleaseNewerWork();
    TestConcurrentRequestStormUsesSingleExecutor();
    return g_failures;
}
