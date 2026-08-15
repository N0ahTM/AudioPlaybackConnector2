#include <core/TrayTooltipBuilder.hpp>

#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, char const* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

void TestEmptyTooltip() {
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", {}, {}, false) == L"App",
          "an empty connection list must return only the app name");
}

void TestNamePrecedenceAndFallbacks() {
    std::vector<DeviceTrayPresentationItem> connected{{L"id-alias", L"Live alias device"},
                                                      {L"id-persisted", L"Live persisted device"},
                                                      {L"id-live", L"Live only"},
                                                      {L"id-only", L""}};
    std::vector<DeviceSettings> settings{{L"id-alias", L"Persisted alias device", L"Alias"},
                                         {L"id-persisted", L"Persisted name", L""}};

    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected, settings, false) ==
              L"App\nAlias\nPersisted name\nLive only\nid-only\n",
          "tooltip names must prefer alias, persisted name, live name, then id");
}

void TestPrivacyAndMissingSettings() {
    std::vector<DeviceTrayPresentationItem> connected{{L"id-alias", L"Live alias device"},
                                                      {L"id-private", L"Private device"}};
    std::vector<DeviceSettings> settings{{L"id-alias", L"", L"Visible alias"}};

    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected, settings, true) == L"App\nVisible alias\nHidden\n",
          "privacy mode must retain aliases and redact every other device name");
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected, {}, false) ==
              L"App\nLive alias device\nPrivate device\n",
          "missing settings must preserve live device names");
}

void TestConnectionOrderAndDuplicates() {
    std::vector<DeviceTrayPresentationItem> connected{{L"id", L"First"}, {L"id", L"Second"}};
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected, {}, false) == L"App\nFirst\nSecond\n",
          "tooltip construction must preserve connection order and duplicate entries");
}

} // namespace

int RunTrayTooltipBuilderTests() {
    TestEmptyTooltip();
    TestNamePrecedenceAndFallbacks();
    TestPrivacyAndMissingSettings();
    TestConnectionOrderAndDuplicates();
    return g_failures;
}
