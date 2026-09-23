#include "TestCheck.hpp"
#include "AppTestFixture.hpp"
#include <ui/DevicePickerViewState.hpp>
#include <algorithm>

namespace {
void TestSingleSnapshotProjectsDiscoverySavedSettingsAndPrivacy() {
    apc::tests::AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"saved", L"Saved device");
    (void)fixture.Settings->SetDeviceAlias(L"saved", L"Desk");
    (void)fixture.Settings->SetDefaultDevice(L"saved");
    (void)fixture.Service->Start();
    auto watcher = fixture.Devices->WatcherAccess->LastWatcher;
    watcher->Add(L"live", L"Visible device");
    watcher->Add(L"live", L"Visible device");
    auto snapshot = fixture.Controller.Snapshot();
    auto view = BuildDevicePickerViewState(snapshot, L"Private");
    auto live = std::ranges::find(view.Items, L"live", &apc::device_picker::DeviceSnapshotItem::Id);
    auto saved = std::ranges::find(view.Items, L"saved", &apc::device_picker::DeviceSnapshotItem::Id);
    Check(view.Items.size() == 2 && live != view.Items.end() && saved != view.Items.end(),
          "one owner snapshot must merge discovery and saved settings without duplicates");
    if (live == view.Items.end() || saved == view.Items.end()) return;
    Check(live->IsAvailable && !saved->IsAvailable && saved->IsDefault && saved->DisplayName == L"Desk",
          "saved unavailable devices remain configurable and retain their alias and default marker");
    auto options = BuildDeviceOptionsViewState(snapshot, L"saved", L"Private");
    Check(options && options->CanForget && options->Device.IsDefaultDevice,
          "inactive saved devices can be forgotten through their options");
    (void)fixture.Controller.SetPrivacyMode(true);
    auto privateSnapshot = fixture.Controller.Snapshot();
    auto privateView = BuildDevicePickerViewState(privateSnapshot, L"Private");
    for (auto const& item : privateView.Items)
        Check(item.DisplayName == (item.Id == L"saved" ? L"Desk" : L"Private"),
              "privacy redacts discovered names while preserving explicit aliases");
    Check(BuildDevicePickerViewState(snapshot, L"Private").Items == view.Items,
          "an immutable snapshot projects identically after the owners change");
    watcher->Remove(L"live");
    watcher->Add(L"saved", L"Rediscovered");
    auto rediscovered = BuildDevicePickerViewState(fixture.Controller.Snapshot(), L"Private");
    Check(rediscovered.Items.size() == 1 && rediscovered.Items.front().IsAvailable,
          "rediscovery updates availability without a second UI inventory or duplicate saved row");
}

void TestMissingPersistedDefaultRemainsResettable() {
    apc::app::AppSnapshot snapshot;
    snapshot.Settings.DefaultDevice = DefaultDeviceMode::SpecificDevice;
    snapshot.Settings.DefaultDeviceId = L"missing";
    snapshot.PrivacyModeEnabled = true;
    auto view = BuildDevicePickerViewState(snapshot, L"Private");
    auto options = BuildDeviceOptionsViewState(snapshot, L"missing", L"Private");
    Check(view.Items.size() == 1 && view.Items.front().IsDefault && !view.Items.front().IsAvailable &&
              view.Items.front().DisplayName == L"Private" && options && options->Device.IsDefaultDevice,
          "an orphaned saved default remains visible and resettable without leaking its ID in privacy mode");
}

void TestLiveActivityAndPolicyComeFromTheSameSnapshot() {
    apc::tests::AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"saved", L"Saved");
    (void)fixture.Settings->SetGlobalConnectOnStartup(true);
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"saved");
    auto snapshot = fixture.Controller.Snapshot();
    auto view = BuildDevicePickerViewState(snapshot, L"Private");
    auto options = BuildDeviceOptionsViewState(snapshot, L"saved", L"Private");
    Check(view.ConnectedDeviceCount == 1 && view.Items.size() == 1 && view.Items.front().IsAvailable,
          "a connected saved device stays visible when discovery is empty");
    Check(options && !options->CanForget && options->GlobalConnectOnStartup,
          "options use the same activity and global policy as the row snapshot");
    Check(!BuildDeviceOptionsViewState(snapshot, L"missing", L"Private"), "missing devices have no options");
    snapshot.IsRunning = false;
    Check(BuildDevicePickerViewState(snapshot, L"Private").Items.empty() &&
              !BuildDeviceOptionsViewState(snapshot, L"saved", L"Private"),
          "unavailable captures must not become actionable UI data");
}
} // namespace

int RunDevicePickerViewStateTests() {
    TestSingleSnapshotProjectsDiscoverySavedSettingsAndPrivacy();
    TestLiveActivityAndPolicyComeFromTheSameSnapshot();
    TestMissingPersistedDefaultRemainsResettable();
    return g_failures;
}
