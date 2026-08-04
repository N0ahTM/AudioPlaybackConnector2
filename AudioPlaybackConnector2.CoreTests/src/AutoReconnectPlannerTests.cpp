#include <app/AutoReconnectPlanner.hpp>

#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

DeviceSettings Device(std::wstring id, bool connectOnStartup = false) {
    DeviceSettings device;
    device.Id = std::move(id);
    device.ConnectOnStartup = connectOnStartup;
    return device;
}

void TestMostRecentlyConnectedPromotionIsIdempotent() {
    std::vector<std::wstring> ids{L"current", L"older"};
    Check(!AutoReconnectPlanner::PromoteMostRecentlyConnected(ids, L"current"),
          "an already unique MRU front must not trigger persistence");
    Check(ids == std::vector<std::wstring>({L"current", L"older"}),
          "an idempotent MRU promotion must preserve ordering");
    Check(!AutoReconnectPlanner::PromoteMostRecentlyConnected(ids, L""), "an empty device id must not alter MRU state");
}

void TestMostRecentlyConnectedPromotionRemovesDuplicates() {
    std::vector<std::wstring> ids{L"a", L"b", L"a", L"c", L"b"};
    Check(AutoReconnectPlanner::PromoteMostRecentlyConnected(ids, L"b"),
          "promoting an older device must report a mutation");
    Check(ids == std::vector<std::wstring>({L"b", L"a", L"a", L"c"}),
          "promotion must remove every duplicate of the promoted id and retain other history");

    Check(AutoReconnectPlanner::PromoteMostRecentlyConnected(ids, L"a"),
          "a duplicated device must be normalized even when it exists in history");
    Check(ids == std::vector<std::wstring>({L"a", L"b", L"c"}),
          "normalization must leave one promoted device and stable relative ordering");
}

void TestReconnectPlanHonorsAllSavedDevicePolicies() {
    SettingsData settings;
    settings.GlobalConnectOnStartup = true;
    settings.Devices = {Device(L"a"), Device(L"b"), Device(L"c")};
    settings.LastConnectedIds = {L"c", L"unknown", L"c", L"a"};

    auto plan = AutoReconnectPlanner::BuildReconnectPlan(settings);
    Check(plan == std::vector<std::wstring>({L"c", L"a", L"b"}),
          "global startup reconnect must include every saved device once while preserving MRU priority");
    Check(AutoReconnectPlanner::HasReconnectTargets(settings),
          "a global policy with saved devices must expose reconnect targets");
}

void TestReconnectPlanHonorsPerDevicePolicyWithoutMruHistory() {
    SettingsData settings;
    settings.Devices = {Device(L"disabled"), Device(L"enabled", true), Device(L"also-enabled", true)};

    auto plan = AutoReconnectPlanner::BuildReconnectPlan(settings);
    Check(plan == std::vector<std::wstring>({L"enabled", L"also-enabled"}),
          "per-device startup policy must not depend on overloaded MRU/session state");

    settings.Devices[1].ConnectOnStartup = false;
    settings.Devices[2].ConnectOnStartup = false;
    Check(!AutoReconnectPlanner::HasReconnectTargets(settings),
          "disabled startup policies must produce no reconnect work");
}

} // namespace

int RunAutoReconnectPlannerTests() {
    TestMostRecentlyConnectedPromotionIsIdempotent();
    TestMostRecentlyConnectedPromotionRemovesDuplicates();
    TestReconnectPlanHonorsAllSavedDevicePolicies();
    TestReconnectPlanHonorsPerDevicePolicyWithoutMruHistory();
    return g_failures;
}
