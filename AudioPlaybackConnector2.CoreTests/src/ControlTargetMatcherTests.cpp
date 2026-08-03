#include <control/ControlTargetMatcher.hpp>

#include <array>
#include <iostream>
#include <string_view>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestExactMatchesOutrankSubstrings() {
    const std::array candidates{
        apc::control::TargetCandidateView{L"id-a", L"Kitchen", L"My Desk Speakers"},
        apc::control::TargetCandidateView{L"id-b", L"Desk", L"Office"},
    };
    auto matches = apc::control::FindAutoTargetMatches(candidates, L"Desk");
    Check(matches.Rank == apc::control::AutoTargetMatchRank::ExactAliasOrName && matches.Indices.size() == 1 &&
              matches.Indices.front() == 1,
          "an exact device name must outrank an alias substring on another device");
}

void TestEqualRankMatchesRemainAmbiguous() {
    const std::array candidates{
        apc::control::TargetCandidateView{L"id-a", L"Kitchen", L"Desk"},
        apc::control::TargetCandidateView{L"id-b", L"Desk", L"Office"},
    };
    auto matches = apc::control::FindAutoTargetMatches(candidates, L"Desk");
    Check(matches.Rank == apc::control::AutoTargetMatchRank::ExactAliasOrName && matches.Indices.size() == 2,
          "alias-exact and name-exact matches on different devices must remain ambiguous");
}

void TestRankOrderAndDeduplication() {
    const std::array candidates{
        apc::control::TargetCandidateView{L"Bluetooth#AABBCCDDEEFF", L"Headset", L"Headset"},
        apc::control::TargetCandidateView{L"Bluetooth#112233445566", L"Other", L"Office Headset"},
    };
    auto exactId = apc::control::FindAutoTargetMatches(candidates, L"Bluetooth#AABBCCDDEEFF");
    Check(exactId.Rank == apc::control::AutoTargetMatchRank::ExactId && exactId.Indices.size() == 1,
          "exact IDs must have the highest automatic target rank");
    auto mac = apc::control::FindAutoTargetMatches(candidates, L"AA:BB:CC:DD:EE:FF");
    Check(mac.Rank == apc::control::AutoTargetMatchRank::MacFragment && mac.Indices.size() == 1,
          "normalized MAC fragments must resolve after exact labels");
    auto substring = apc::control::FindAutoTargetMatches(candidates, L"set");
    Check(substring.Rank == apc::control::AutoTargetMatchRank::AliasOrNameSubstring && substring.Indices.size() == 2,
          "same-rank alias/name substring matches must remain ambiguous without duplicate candidate entries");
}
} // namespace

int RunControlTargetMatcherTests() {
    TestExactMatchesOutrankSubstrings();
    TestEqualRankMatchesRemainAmbiguous();
    TestRankOrderAndDeduplication();
    return g_failures;
}
