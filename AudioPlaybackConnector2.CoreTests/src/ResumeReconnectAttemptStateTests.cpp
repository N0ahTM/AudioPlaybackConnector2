#include "TestCheck.hpp"

#include <app/ResumeReconnectAttemptState.hpp>
#include <string>
#include <vector>

namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// ResumeReconnectAttemptState Tests /////////////////////////////////////////////////////////////////*/
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

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Test Entry Point //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunResumeReconnectAttemptStateTests() {
    TestResumeReconnectCountsOnlyActuallyStartedAttempts();
    TestResumeReconnectPreservesPendingTargetsAcrossSuspendCycles();
    return g_failures;
}
