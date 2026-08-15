#include <app/LatestStartupTaskRequestState.hpp>

#include <algorithm>
#include <array>
#include <barrier>
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

void TestInitialDesiredRequestStartsOneOperation() {
    LatestStartupTaskRequestState state;
    auto request = state.RequestDesired(true);

    Check(request.Accepted, "the first desired request must be accepted");
    Check(!request.Coalesced, "the first desired request must not be coalesced");
    Check(request.Revision == 1, "the first desired request must receive revision one");
    Check(request.OperationToStart.has_value(), "the first desired request must start one operation");
    Check(request.OperationToStart &&
              request.OperationToStart->Kind == LatestStartupTaskRequestState::RequestKind::Desired,
          "a desired request must preserve its kind");
    Check(request.OperationToStart && request.OperationToStart->Desired,
          "a desired request must preserve its desired value");
    Check(state.InFlight() == request.OperationToStart, "the started operation must be the only in-flight owner");
}

void TestSameDesiredRequestCoalesces() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestDesired(true);
    auto duplicateWhileRunning = state.RequestDesired(true);

    Check(duplicateWhileRunning.Accepted && duplicateWhileRunning.Coalesced,
          "the same desired value must coalesce while its operation is running");
    Check(duplicateWhileRunning.Revision == first.Revision,
          "a coalesced desired request must retain the represented revision");
    Check(!duplicateWhileRunning.OperationToStart, "a coalesced desired request must not start a duplicate operation");

    auto completion = state.Complete(*first.OperationToStart);
    Check(completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the represented desired operation must publish when it completes");

    auto duplicateAfterPublication = state.RequestDesired(true);
    Check(duplicateAfterPublication.Accepted && duplicateAfterPublication.Coalesced,
          "the already-published desired value must remain coalesced until a refresh or new intent");
    Check(duplicateAfterPublication.Revision == first.Revision,
          "coalescing an already-published desired value must not advance the revision");
    Check(!state.InFlight(), "coalescing an already-published desired value must remain idle");
}

void TestDifferentDesiredRequestSupersedesInFlightWork() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestDesired(true);
    auto latest = state.RequestDesired(false);

    Check(latest.Accepted && !latest.Coalesced, "a different desired value must record a new intent");
    Check(latest.Revision == first.Revision + 1, "a different desired value must advance the revision");
    Check(!latest.OperationToStart, "a different desired value must not overlap the active operation");
    Check(state.InFlight() == first.OperationToStart, "the original operation must remain the sole in-flight owner");

    auto oldCompletion = state.Complete(*first.OperationToStart);
    Check(oldCompletion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Superseded,
          "completion of an older intent must never publish");
    Check(oldCompletion.OperationToStart.has_value(), "the latest intent must start after the old operation completes");
    Check(oldCompletion.OperationToStart && oldCompletion.OperationToStart->Revision == latest.Revision,
          "the follow-up operation must represent the latest revision");
    Check(oldCompletion.OperationToStart && !oldCompletion.OperationToStart->Desired,
          "the follow-up operation must preserve the latest desired value");

    auto latestCompletion = state.Complete(*oldCompletion.OperationToStart);
    Check(latestCompletion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the latest desired operation must publish");
}

void TestLastIntentWinsAcrossSeveralReplacements() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestDesired(true);
    auto second = state.RequestDesired(false);
    auto third = state.RequestRefresh();
    auto latest = state.RequestDesired(true);

    Check(first.Revision < second.Revision && second.Revision < third.Revision && third.Revision < latest.Revision,
          "distinct intents must receive strictly increasing revisions");
    auto superseded = state.Complete(*first.OperationToStart);
    Check(superseded.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Superseded,
          "an operation with several replacements must be superseded");
    Check(superseded.OperationToStart && superseded.OperationToStart->Revision == latest.Revision,
          "only the final intent must be scheduled after several replacements");
    Check(superseded.OperationToStart &&
              superseded.OperationToStart->Kind == LatestStartupTaskRequestState::RequestKind::Desired &&
              superseded.OperationToStart->Desired,
          "the final desired intent must replace an intermediate refresh");
}

void TestRefreshCoalescesOnlyWhileUnsettled() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestRefresh();
    auto duplicate = state.RequestRefresh();

    Check(duplicate.Accepted && duplicate.Coalesced && duplicate.Revision == first.Revision,
          "concurrent refresh requests must share one unsettled operation");
    Check(!duplicate.OperationToStart, "an unsettled refresh must not start duplicate work");
    Check(state.Complete(*first.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the shared refresh operation must publish once");

    auto later = state.RequestRefresh();
    Check(later.Accepted && !later.Coalesced && later.Revision > first.Revision,
          "a refresh requested after publication must create a new revision");
    Check(later.OperationToStart.has_value(), "a later refresh must start a new operation while idle");
}

void TestStaleAndDuplicateCompletionsAreIgnored() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestRefresh();
    auto wrong = *first.OperationToStart;
    ++wrong.Revision;

    Check(state.Complete(wrong).Disposition == LatestStartupTaskRequestState::CompletionDisposition::Stale,
          "a completion with the wrong token must be stale");
    Check(state.InFlight() == first.OperationToStart, "a stale completion must not consume the active operation");
    Check(state.Complete(*first.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the real operation must survive a stale completion");
    Check(state.Complete(*first.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Stale,
          "a duplicate completion must be stale");
}

void TestStopSuppressesPublicationAndFutureRequests() {
    LatestStartupTaskRequestState state;
    auto request = state.RequestDesired(true);
    state.Stop();

    Check(state.Stopped(), "stop must be observable");
    Check(!state.InFlight(), "stop must invalidate the active operation");
    Check(state.Complete(*request.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Stale,
          "completion after stop must never publish");

    auto desired = state.RequestDesired(false);
    auto refresh = state.RequestRefresh();
    Check(!desired.Accepted && !desired.OperationToStart, "stop must reject later desired requests");
    Check(!refresh.Accepted && !refresh.OperationToStart, "stop must reject later refresh requests");
    Check(state.Revision() == request.Revision, "rejected requests after stop must not advance revisions");
    state.Stop();
    Check(state.Stopped(), "stop must be idempotent");
}

void TestConcurrentRequestsStartAtMostOneOperation() {
    constexpr std::size_t c_threadCount = 32;
    LatestStartupTaskRequestState state;
    std::barrier start{c_threadCount};
    std::array<LatestStartupTaskRequestState::RequestResult, c_threadCount> results{};
    std::vector<std::jthread> threads;
    threads.reserve(c_threadCount);
    for (std::size_t index = 0; index < c_threadCount; ++index) {
        threads.emplace_back([&, index]() {
            start.arrive_and_wait();
            results[index] = state.RequestDesired((index % 2) == 0);
        });
    }
    threads.clear();

    auto const operationsStarted =
        std::ranges::count_if(results, [](auto const& result) { return result.OperationToStart.has_value(); });
    Check(operationsStarted == 1, "concurrent requests must start at most one operation");
    Check(state.InFlight().has_value(), "concurrent requests must leave exactly one operation in flight");

    std::vector<std::uint64_t> distinctRevisions;
    for (auto const& result : results) {
        if (std::ranges::find(distinctRevisions, result.Revision) == distinctRevisions.end()) {
            distinctRevisions.push_back(result.Revision);
        }
    }
    std::ranges::sort(distinctRevisions);
    bool revisionsAreMonotone = !distinctRevisions.empty() && distinctRevisions.front() == 1;
    for (std::size_t index = 1; index < distinctRevisions.size(); ++index) {
        revisionsAreMonotone = revisionsAreMonotone && distinctRevisions[index] == distinctRevisions[index - 1] + 1;
    }
    Check(revisionsAreMonotone, "concurrent distinct intents must allocate a gap-free monotone revision sequence");

    auto firstCompletion = state.Complete(*state.InFlight());
    Check(firstCompletion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish ||
              (firstCompletion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Superseded &&
               firstCompletion.OperationToStart.has_value()),
          "the sole concurrent operation must publish or hand off to exactly one latest replacement");
    if (firstCompletion.OperationToStart) {
        Check(state.Complete(*firstCompletion.OperationToStart).Disposition ==
                  LatestStartupTaskRequestState::CompletionDisposition::Publish,
              "the latest concurrent replacement must publish after the handoff");
    }
}

void TestRequestCompletionRaceNeverLosesTheLatestIntent() {
    constexpr std::size_t c_iterations = 128;
    for (std::size_t iteration = 0; iteration < c_iterations; ++iteration) {
        LatestStartupTaskRequestState state;
        auto initial = state.RequestDesired(false);
        std::barrier raceStart{2};
        LatestStartupTaskRequestState::CompletionResult completion;
        LatestStartupTaskRequestState::RequestResult request;

        std::jthread completionThread([&]() {
            raceStart.arrive_and_wait();
            completion = state.Complete(*initial.OperationToStart);
        });
        std::jthread requestThread([&]() {
            raceStart.arrive_and_wait();
            request = state.RequestDesired(true);
        });
        completionThread.join();
        requestThread.join();

        if (completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish) {
            Check(request.OperationToStart && request.OperationToStart->Revision == request.Revision,
                  "a request after publication must start its own latest operation");
        } else {
            Check(completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Superseded &&
                      completion.OperationToStart && completion.OperationToStart->Revision == request.Revision,
                  "a request before completion must receive the single handoff operation");
        }

        auto latest = state.InFlight();
        Check(latest && latest->Revision == request.Revision && latest->Desired,
              "the raced state must retain the newest desired intent");
        if (latest) {
            Check(state.Complete(*latest).Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish,
                  "the newest raced intent must be publishable");
        }
    }
}

} // namespace

int RunLatestStartupTaskRequestStateTests() {
    TestInitialDesiredRequestStartsOneOperation();
    TestSameDesiredRequestCoalesces();
    TestDifferentDesiredRequestSupersedesInFlightWork();
    TestLastIntentWinsAcrossSeveralReplacements();
    TestRefreshCoalescesOnlyWhileUnsettled();
    TestStaleAndDuplicateCompletionsAreIgnored();
    TestStopSuppressesPublicationAndFutureRequests();
    TestConcurrentRequestsStartAtMostOneOperation();
    TestRequestCompletionRaceNeverLosesTheLatestIntent();
    return g_failures;
}
