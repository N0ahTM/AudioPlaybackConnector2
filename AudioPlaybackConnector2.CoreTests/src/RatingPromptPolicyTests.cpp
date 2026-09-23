#include "TestCheck.hpp"

#include <app/RatingPromptPolicy.hpp>

#include <string>

namespace {

RatingPromptData MatureData() {
    RatingPromptData data;
    data.FirstLaunchDate = L"2026-09-01";
    data.UsageDays = 3;
    data.LastUsageDate = L"2026-09-20";
    return data;
}

void TestEligibilityRequiresAgeAndUsage() {
    Check(apc::app::IsRatingPromptEligible(MatureData(), L"2026-09-22"),
          "mature usage must be eligible after package feature admission");

    // 14-day minimum since first launch.
    auto young = MatureData();
    young.FirstLaunchDate = L"2026-09-09";
    Check(!apc::app::IsRatingPromptEligible(young, L"2026-09-22"), "13 days must be too early");
    young.FirstLaunchDate = L"2026-09-08";
    Check(apc::app::IsRatingPromptEligible(young, L"2026-09-22"), "exactly 14 days must be eligible");

    // Three usage days minimum.
    auto barelyUsed = MatureData();
    barelyUsed.UsageDays = 2;
    Check(!apc::app::IsRatingPromptEligible(barelyUsed, L"2026-09-22"), "two usage days must not suffice");

    // Missing or malformed dates fail closed.
    auto unknown = MatureData();
    unknown.FirstLaunchDate = L"";
    Check(!apc::app::IsRatingPromptEligible(unknown, L"2026-09-22"), "a missing first-launch date must fail closed");
    unknown.FirstLaunchDate = L"2026-13-40";
    Check(!apc::app::IsRatingPromptEligible(unknown, L"2026-09-22"), "a malformed date must fail closed");
    unknown.FirstLaunchDate = L"2026-02-31";
    Check(!apc::app::IsRatingPromptEligible(unknown, L"2026-09-22"), "an impossible calendar day must fail closed");
    Check(!apc::app::IsRatingPromptEligible(MatureData(), L"2026-09-2x"), "today must contain only date digits");
}

void TestAskedStaysSilent() {
    auto asked = MatureData();
    asked.Asked = true;
    Check(!apc::app::IsRatingPromptEligible(asked, L"2026-09-22"), "an already-presented prompt must stay silent");
}

void TestTodayIsIsoLocalDay() {
    const auto today = apc::app::TodayLocalIsoDate();
    Check(today.size() == 10 && today[4] == L'-' && today[7] == L'-', "today must be an ISO local day");
}

} // namespace

int RunRatingPromptPolicyTests() {
    TestEligibilityRequiresAgeAndUsage();
    TestAskedStaysSilent();
    TestTodayIsIsoLocalDay();
    return g_failures;
}
