#include "TestCheck.hpp"

#include <app/LatestStartupTaskRequestState.hpp>

#include <initializer_list>

namespace {

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
    Check(!duplicateAfterPublication.OperationToStart,
          "coalescing an already-published desired value must remain idle");
}

void TestDifferentDesiredRequestSupersedesInFlightWork() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestDesired(true);
    auto latest = state.RequestDesired(false);

    Check(latest.Accepted && !latest.Coalesced, "a different desired value must record a new intent");
    Check(latest.Revision == first.Revision + 1, "a different desired value must advance the revision");
    Check(!latest.OperationToStart, "a different desired value must not overlap the active operation");

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
    auto third = state.RequestDesired(true);
    auto latest = state.RequestDesired(false);

    Check(first.Revision < second.Revision && second.Revision < third.Revision && third.Revision < latest.Revision,
          "distinct intents must receive strictly increasing revisions");
    auto superseded = state.Complete(*first.OperationToStart);
    Check(superseded.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Superseded,
          "an operation with several replacements must be superseded");
    Check(superseded.OperationToStart && superseded.OperationToStart->Revision == latest.Revision,
          "only the final intent must be scheduled after several replacements");
    Check(superseded.OperationToStart &&
              superseded.OperationToStart->Kind == LatestStartupTaskRequestState::RequestKind::Desired &&
              !superseded.OperationToStart->Desired,
          "the final desired intent must replace every intermediate desired value");
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

void TestRefreshDoesNotSupersedeUnsettledDesiredIntent() {
    LatestStartupTaskRequestState state;
    auto desired = state.RequestDesired(true);
    auto refresh = state.RequestRefresh();

    Check(refresh.Accepted && refresh.Coalesced, "refresh must join rather than supersede an unsettled desired intent");
    Check(refresh.Revision == desired.Revision, "refresh joining a desired intent must retain the desired revision");
    Check(!refresh.OperationToStart, "refresh must not start work beside an unsettled desired operation");
    Check(state.Complete(*desired.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the desired operation must remain publishable after a refresh request");
}

void TestUnsatisfiedDesiredIntentCanBeRetried() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestDesired(true);
    auto failedCompletion = state.Complete(*first.OperationToStart, false);

    Check(failedCompletion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "an unsatisfied latest operation must still allow its authoritative result to publish");
    auto retry = state.RequestDesired(true);
    Check(retry.Accepted && !retry.Coalesced,
          "an unsatisfied desired intent must allow the same desired value to be retried");
    Check(retry.Revision == first.Revision + 1, "retrying an unsatisfied desired intent must advance the revision");
    Check(retry.OperationToStart && retry.OperationToStart->Desired,
          "retrying an unsatisfied desired intent must start the requested operation again");
}

void TestStaleAndDuplicateCompletionsAreIgnored() {
    LatestStartupTaskRequestState state;
    auto first = state.RequestRefresh();
    auto wrong = *first.OperationToStart;
    ++wrong.Revision;

    Check(state.Complete(wrong).Disposition == LatestStartupTaskRequestState::CompletionDisposition::Stale,
          "a completion with the wrong token must be stale");
    Check(state.Complete(*first.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Publish,
          "the real operation must survive a stale completion");
    Check(state.Complete(*first.OperationToStart).Disposition ==
              LatestStartupTaskRequestState::CompletionDisposition::Stale,
          "a duplicate completion must be stale");
}

void TestRequestAndCompletionInEitherSerialOrder() {
    for (bool requestFirst : {false, true}) {
        LatestStartupTaskRequestState state;
        auto initial = state.RequestDesired(false);
        LatestStartupTaskRequestState::RequestResult request;
        if (requestFirst) request = state.RequestDesired(true);
        auto completion = state.Complete(*initial.OperationToStart);
        if (!requestFirst) request = state.RequestDesired(true);
        auto latest = requestFirst ? completion.OperationToStart : request.OperationToStart;
        Check(latest && latest->Revision == request.Revision && latest->Desired,
              "either serialized ordering must schedule the latest intent exactly once");
        if (latest) {
            Check(state.Complete(*latest).Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish,
                  "the latest intent must publish after either ordering");
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
    TestRefreshDoesNotSupersedeUnsettledDesiredIntent();
    TestUnsatisfiedDesiredIntentCanBeRetried();
    TestStaleAndDuplicateCompletionsAreIgnored();
    TestRequestAndCompletionInEitherSerialOrder();
    return g_failures;
}
