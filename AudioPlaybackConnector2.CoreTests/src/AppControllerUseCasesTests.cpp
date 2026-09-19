#include "TestCheck.hpp"
#include "AppTestFixture.hpp"
#include <core/SettingsLimits.hpp>

namespace {
using namespace apc::app;
using apc::tests::AppFixture;

DeviceSelector Query(DeviceSelectorKind kind, std::wstring_view text) {
    return *DeviceSelector::ByQuery(kind, text);
}

void TestTargetResolutionAndDefaultModes() {
    AppFixture fixture;
    auto& owner = *fixture.Settings;
    auto& controller = fixture.Controller;
    (void)owner.RememberDevice(L"bluetooth-AABBCC001122", L"Headphones");
    (void)owner.RememberDevice(L"second", L"Headphones Max");
    (void)owner.RememberDevice(L"third", L"Speaker");
    (void)owner.SetDeviceAlias(L"bluetooth-AABBCC001122", L"Desk");
    (void)owner.SetDeviceAlias(L"third", L"Desk Max");
    for (auto const& selector : {Query(DeviceSelectorKind::Name, L"HEADPHONES"),
                                 Query(DeviceSelectorKind::Alias, L"desk"),
                                 Query(DeviceSelectorKind::Mac, L"AA:BB:CC"),
                                 Query(DeviceSelectorKind::Auto, L"bluetooth-AABBCC001122"),
                                 Query(DeviceSelectorKind::Auto, L"Headphones")}) {
        auto result = controller.SetDefault(selector, {});
        Check(result.Succeeded() && owner.Snapshot().Data.DefaultDeviceId == L"bluetooth-AABBCC001122",
              "explicit and automatic selectors must preserve exact-match precedence, case folding and MAC "
              "normalization");
    }
    Check(controller.SetDefault(Query(DeviceSelectorKind::Name, L"Head"), {}).Code == AppResultCode::Ambiguous &&
              controller.SetDefault(Query(DeviceSelectorKind::Alias, L"Des"), {}).Code == AppResultCode::Ambiguous,
          "equal-rank name and alias matches must fail as ambiguous");
    Check(controller.SetDefault(Query(DeviceSelectorKind::Mac, L"AABB"), {}).Code == AppResultCode::NotFound &&
              controller.SetDefault(*DeviceSelector::ById(L"missing"), {}).Code == AppResultCode::NotFound,
          "short MAC fragments and absent persistent targets must not invent a match");
    (void)owner.RecordConnectedDevice(L"third", L"Speaker");
    auto last = controller.Connect(DeviceSelector::Last(), AppCommandContext::Detached());
    Check(last.Succeeded() && fixture.Devices->Service.Snapshot().Sessions.front().DeviceId == L"third",
          "Last must use the settings owner's recorded identity");
    Check(controller.ClearDefault().Succeeded() &&
              controller.ShowDefault({}).DefaultDevice->Mode == apc::app::DefaultDeviceMode::LastConnected,
          "clearing the specific default must restore last-connected selection");
    AppFixture empty;
    Check(empty.Controller.Connect(DeviceSelector::Default(), {}).Reason == AppOutcomeReason::DefaultTargetMissing &&
              empty.Controller.Connect(DeviceSelector::Last(), {}).Reason == AppOutcomeReason::LastTargetMissing,
          "missing default and last selectors must retain distinct typed reasons");
}

void TestSettingsResultsReadTheCommittedOwner() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    (void)fixture.Settings->RememberDevice(L"other", L"Other");
    auto subscription = fixture.Settings->Subscribe([&](SettingsSnapshot const& snapshot) {
        if (snapshot.Data.DefaultDeviceId == L"target") (void)fixture.Settings->SetDefaultDevice(L"other");
        auto found = std::ranges::find(snapshot.Data.Devices, std::wstring{L"target"}, &DeviceSettings::Id);
        if (found != snapshot.Data.Devices.end() && found->Alias == L"Requested")
            (void)fixture.Settings->SetDeviceAlias(L"target", L"Committed");
    });
    auto result = fixture.Controller.SetDefault(L"target");
    Check(result.Succeeded() && result.DefaultDevice && result.DefaultDevice->Id &&
              result.DefaultDevice->Id->View() == L"other",
          "default results must reread the committed store after reentrant mutation");
    result = fixture.Controller.SetAlias(L"target", L"Requested");
    Check(result.Succeeded() && result.Alias == L"Committed" && result.Device && result.Device->Alias == L"Committed",
          "alias results must expose the committed owner value, not echo the request");
    subscription.Reset();
    (void)fixture.Settings->SetPrivacyModeEnabled(true);
    auto snapshot = fixture.Controller.Snapshot();
    Check(snapshot.PrivacyModeEnabled && snapshot.Devices.size() == 2,
          "privacy and device labels must come from the current settings owner");
    (void)fixture.Settings->Shutdown(SettingsShutdownMode::DiscardStartupFailure);
    Check(fixture.Controller.SetAlias(L"target", L"Rejected").Code == AppResultCode::OperationFailed,
          "a rejected store mutation must not report success");
}

void TestRefreshCancellationCannotAdmitMutation() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    std::stop_source stop;
    fixture.Devices->WatcherAccess->BeforeRefreshCompletion = [&] { stop.request_stop(); };
    auto const revision = fixture.Settings->Snapshot().Revision;
    AppCommandContext context{stop.get_token(), AppCommandContext::TimePoint::max()};
    auto result = fixture.Controller.SetDefault(Query(DeviceSelectorKind::Name, L"Target"), context);
    Check(result.Code == AppResultCode::Cancelled && fixture.Settings->Snapshot().Revision == revision,
          "cancellation during refresh must be rechecked before committing a setting");
    Check(fixture.Devices->ConnectionAccess->Connections.empty(),
          "refresh cancellation must not enter device mutation");
}

void TestRefreshFailureRetainsKnownTargets() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    fixture.Devices->WatcherAccess->BeforeRefreshCompletion = [] { throw winrt::hresult_error(E_ACCESSDENIED); };
    auto result = fixture.Controller.SetDefault(Query(DeviceSelectorKind::Name, L"Target"), {});
    Check(result.Succeeded() && fixture.Settings->Snapshot().Data.DefaultDeviceId == L"target",
          "failed Windows discovery must retain known settings targets");
}

void TestRefreshReadsCurrentSessionsAfterEnumeration() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
    fixture.Devices->WatcherAccess->BeforeRefreshCompletion = [&] {
        (void)fixture.Service->Disconnect(L"target");
        apc::tests::device::CompleteCloseAndCooldown(*fixture.Devices,
                                                     fixture.Devices->ConnectionAccess->LastConnection);
    };
    auto result = fixture.Controller.ListDevices({});
    Check(result.Succeeded() && result.Snapshot && result.Snapshot->Devices.size() == 1 &&
              !result.Snapshot->Devices.front().IsConnected && !result.Snapshot->Devices.front().IsBusy,
          "a disconnect during discovery must be reflected by the returned session snapshot");
}

void TestOpaqueExternalIdentityAndIdempotency() {
    AppFixture fixture;
    std::wstring longId(513, L'x');
    auto selector = *DeviceSelector::ById(longId);
    auto admitted = fixture.Controller.Connect(selector, AppCommandContext::Detached());
    Check(admitted.Succeeded(), "a valid external ID must not be limited by the persistence bound");
    auto* connection = fixture.Devices->ConnectionAccess->LastConnection;
    connection->CompleteStart(apc::device::DeviceConnectionResult::Success);
    connection->CompleteOpen(apc::device::DeviceConnectionResult::Success);
    auto connected = fixture.Controller.Connect(selector, {});
    Check(connected.Succeeded() && connected.Reason == AppOutcomeReason::AlreadyConnected &&
              fixture.Devices->ConnectionAccess->Connections.size() == 1,
          "connect on an established session must be idempotent");
    Check(fixture.Controller.Snapshot().Devices.front().Id.View() == longId,
          "application snapshots must preserve the external ID in full");
    Check(fixture.Controller.SetDefault(longId).Succeeded() &&
              fixture.Settings->Snapshot().Data.DefaultDeviceId.empty(),
          "the wire-compatible long-ID default outcome must not force an invalid persistent identity");
    auto disconnected = fixture.Controller.Disconnect(selector, {});
    Check(disconnected.Succeeded(), "disconnect must report the close transition");
    apc::tests::device::CompleteCloseAndCooldown(*fixture.Devices, connection);
    Check(fixture.Controller.Disconnect(selector, {}).Reason == AppOutcomeReason::AlreadyDisconnected,
          "disconnect on an idle session must be idempotent");
}

void TestDetachedToggleRespectsOwnedBusyState() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    (void)fixture.Settings->SetDefaultDevice(L"target");
    auto context = AppCommandContext::Detached();
    Check(fixture.Controller.Connect(*DeviceSelector::ById(L"target"), context).Succeeded(),
          "fixture must start connecting");
    auto result = fixture.Controller.ToggleDefault(context);
    Check(result.Code == AppResultCode::Busy && fixture.Devices->ConnectionAccess->Connections.size() == 1,
          "tray toggle must use current device-owner busy state without starting a competing operation");
}

} // namespace

int RunAppControllerUseCasesTests() {
    TestTargetResolutionAndDefaultModes();
    TestSettingsResultsReadTheCommittedOwner();
    TestRefreshCancellationCannotAdmitMutation();
    TestRefreshFailureRetainsKnownTargets();
    TestRefreshReadsCurrentSessionsAfterEnumeration();
    TestOpaqueExternalIdentityAndIdempotency();
    TestDetachedToggleRespectsOwnedBusyState();
    return g_failures;
}
