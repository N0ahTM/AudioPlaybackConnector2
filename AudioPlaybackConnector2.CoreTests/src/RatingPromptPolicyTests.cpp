#include "TestCheck.hpp"

#include <app/RatingPromptPolicy.hpp>

#include <string>

namespace {

using apc::app::RatingPromptOutcome;

RatingPromptData MatureData() {
    RatingPromptData data;
    data.FirstLaunchDate = L"2026-09-01";
    data.UsageDays = 3;
    data.LastUsageDate = L"2026-09-20";
    return data;
}

void TestEligibilityRequiresChannelAgeAndUsage() {
    // Everything satisfied: eligible only in the Store channel.
    Check(apc::app::IsRatingPromptEligible(true, MatureData(), L"2026-09-22"),
          "a mature Store install must be eligible");
    Check(!apc::app::IsRatingPromptEligible(false, MatureData(), L"2026-09-22"),
          "the GitHub channel must never show the prompt");

    // 14-day minimum since first launch.
    auto young = MatureData();
    young.FirstLaunchDate = L"2026-09-09";
    Check(!apc::app::IsRatingPromptEligible(true, young, L"2026-09-22"), "13 days must be too early");
    young.FirstLaunchDate = L"2026-09-08";
    Check(apc::app::IsRatingPromptEligible(true, young, L"2026-09-22"), "exactly 14 days must be eligible");

    // Three usage days minimum.
    auto barelyUsed = MatureData();
    barelyUsed.UsageDays = 2;
    Check(!apc::app::IsRatingPromptEligible(true, barelyUsed, L"2026-09-22"), "two usage days must not suffice");

    // Missing or malformed dates fail closed.
    auto unknown = MatureData();
    unknown.FirstLaunchDate = L"";
    Check(!apc::app::IsRatingPromptEligible(true, unknown, L"2026-09-22"),
          "a missing first-launch date must fail closed");
    unknown.FirstLaunchDate = L"2026-13-40";
    Check(!apc::app::IsRatingPromptEligible(true, unknown, L"2026-09-22"), "a malformed date must fail closed");
}

void TestTerminalStatesStaySilent() {
    auto completed = MatureData();
    completed.State = RatingPromptState::Completed;
    Check(!apc::app::IsRatingPromptEligible(true, completed, L"2026-09-22"), "completed must stay silent");
    auto disabled = MatureData();
    disabled.State = RatingPromptState::Disabled;
    Check(!apc::app::IsRatingPromptEligible(true, disabled, L"2026-09-22"), "disabled must stay silent");
}

void TestDeferralWindow() {
    auto deferred = MatureData();
    deferred.State = RatingPromptState::Deferred;
    deferred.DeferUntil = L"2026-10-06";
    deferred.DeferCount = 1;
    Check(!apc::app::IsRatingPromptEligible(true, deferred, L"2026-10-05"), "the deferral window must hide the prompt");
    Check(apc::app::IsRatingPromptEligible(true, deferred, L"2026-10-06"), "the deferral day itself must show again");
    Check(apc::app::IsRatingPromptEligible(true, deferred, L"2026-10-20"), "a later day must show again");
}

void TestOutcomeTransitions() {
    auto rated = apc::app::ApplyRatingPromptOutcome(RatingPromptOutcome::Rated, MatureData(), L"2026-09-22");
    Check(rated.State == RatingPromptState::Completed, "rating must complete the prompt");

    auto dismissed = apc::app::ApplyRatingPromptOutcome(RatingPromptOutcome::Dismissed, MatureData(), L"2026-09-22");
    Check(dismissed.State == RatingPromptState::Disabled, "dismissal must disable the prompt");

    auto firstDefer = apc::app::ApplyRatingPromptOutcome(RatingPromptOutcome::Deferred, MatureData(), L"2026-09-22");
    Check(firstDefer.State == RatingPromptState::Deferred && firstDefer.DeferUntil == L"2026-10-06" &&
              firstDefer.DeferCount == 1,
          "the first deferral must move the prompt exactly 14 days out");

    auto secondDefer = apc::app::ApplyRatingPromptOutcome(RatingPromptOutcome::Deferred, firstDefer, L"2026-10-06");
    Check(secondDefer.State == RatingPromptState::Disabled, "a second deferral must disable the prompt permanently");
}

void TestIsoDateArithmetic() {
    Check(apc::app::AddDaysToIsoDate(L"2026-02-27", 2) == L"2026-03-01", "month rollover must work");
    Check(apc::app::AddDaysToIsoDate(L"2024-02-28", 1) == L"2024-02-29", "leap years must work");
    Check(apc::app::AddDaysToIsoDate(L"2025-12-31", 14) == L"2026-01-14", "year rollover must work");
    Check(apc::app::AddDaysToIsoDate(L"not-a-date", 14).empty(), "invalid dates must yield an empty result");
    const auto today = apc::app::TodayLocalIsoDate();
    Check(today.size() == 10 && today[4] == L'-' && today[7] == L'-', "today must be an ISO local day");
}

} // namespace

int RunRatingPromptPolicyTests() {
    TestEligibilityRequiresChannelAgeAndUsage();
    TestTerminalStatesStaySilent();
    TestDeferralWindow();
    TestOutcomeTransitions();
    TestIsoDateArithmetic();
    return g_failures;
}
