#include "TestCheck.hpp"
#include "AppTestFixture.hpp"
#include <core/TrayTooltipBuilder.hpp>

namespace {
void TestSnapshotLabelsAndPrivacy() {
    apc::tests::AppFixture fixture;
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", fixture.Controller.Snapshot()) == L"App",
          "a tray without connections must show only the application name");
    (void)fixture.Settings->RememberDevice(L"device", L"Saved name");
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"device");
    auto const connected = fixture.Controller.Snapshot();
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected) == L"App\nSaved name\n" &&
              !connected.Tray.HasBusyOperations,
          "the tray must use the same persisted label and connected state as the application snapshot");
    (void)fixture.Controller.SetPrivacyMode(true);
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", fixture.Controller.Snapshot()) == L"App\nHidden\n",
          "privacy must redact names using the same snapshot as the connections");
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", connected) == L"App\nSaved name\n",
          "a retained snapshot must not perform another settings read during rendering");
    (void)fixture.Controller.SetAlias(L"device", L"Desk");
    Check(apc::tray::BuildTooltip(L"App", L"Hidden", fixture.Controller.Snapshot()) == L"App\nDesk\n",
          "explicit aliases remain visible in privacy mode");
    (void)fixture.Controller.Disconnect(*apc::app::DeviceSelector::ById(L"device"),
                                        apc::app::AppCommandContext::Detached());
    auto const disconnecting = fixture.Controller.Snapshot();
    Check(disconnecting.Tray.HasBusyOperations && apc::tray::BuildTooltip(L"App", L"Hidden", disconnecting) == L"App",
          "disconnection must remove the label while retaining the busy indicator in the same snapshot");
}
} // namespace

int RunTrayTooltipBuilderTests() {
    TestSnapshotLabelsAndPrivacy();
    return g_failures;
}
