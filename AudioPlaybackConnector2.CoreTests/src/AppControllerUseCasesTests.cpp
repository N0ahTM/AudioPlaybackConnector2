#include "TestCheck.hpp"
#include "AppTestFixture.hpp"
#include <core/SettingsLimits.hpp>
#include <ui/DevicePickerViewState.hpp>
#include <array>
#include <atomic>
#include <future>
#include <semaphore>
#include <utility>

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

void TestExplicitSettingsInputContracts() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    auto const target = *DeviceSelector::ById(L"target");
    auto const revision = fixture.Settings->Snapshot().Revision;
    for (auto const& alias : {std::wstring{},
                              std::wstring{L"line\nwrapped"},
                              std::wstring{L"line\rwrapped"},
                              std::wstring{L"abc\0nul", 7},
                              std::wstring(c_maxAppCommandTextCharacters + 1, L'x')}) {
        Check(fixture.Controller.SetAlias(target, alias, {}).Code == AppResultCode::InvalidInput,
              "the explicit alias action must reject malformed input before accessing the store");
    }
    for (auto const& implicit : {DeviceSelector::Default(), DeviceSelector::Last()}) {
        Check(fixture.Controller.SetDefault(implicit, {}).Code == AppResultCode::InvalidInput &&
                  fixture.Controller.SetAlias(implicit, L"Alias", {}).Code == AppResultCode::InvalidInput &&
                  fixture.Controller.ClearAlias(implicit, {}).Code == AppResultCode::InvalidInput,
              "persistent mutations must require an explicit target");
    }
    for (auto const& alias : {std::wstring(129, L'x'), std::wstring(1, static_cast<wchar_t>(0xD800))}) {
        auto const result = fixture.Controller.SetAlias(target, alias, {});
        Check(result.Code == AppResultCode::OperationFailed && result.Reason == AppOutcomeReason::AliasSetFailed,
              "transport-valid aliases outside the persistence format must retain the typed mutation failure");
    }
    Check(fixture.Settings->Snapshot().Revision == revision,
          "rejected alias and default inputs must not commit or advance the settings revision");
    Check(fixture.Controller.SetAlias(target, L"Living room", {}).Succeeded() &&
              fixture.Controller.SetAlias(L"target", L"").Succeeded() &&
              fixture.Settings->Snapshot().Data.Devices.front().Alias.empty(),
          "the exact-ID UI overload must preserve clearing an alias with empty text");
}

void TestQueriesChooseTheirRequiredInputs() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    int refreshes = 0;
    fixture.Devices->WatcherAccess->BeforeRefreshCompletion = [&] { ++refreshes; };
    auto const status = fixture.Controller.Status({});
    auto const aliases = fixture.Controller.ListAliases({});
    auto const selected = fixture.Controller.ShowDefault({});
    Check(status.Succeeded() && aliases.Succeeded() && selected.Succeeded() && refreshes == 0,
          "status, aliases and default queries must read current owner snapshots without discovery");
    auto const devices = fixture.Controller.ListDevices({});
    Check(devices.Succeeded() && refreshes == 1 && devices.Devices == status.Devices &&
              status.Command == AppCommandKind::Status && aliases.Command == AppCommandKind::ListAliases &&
              selected.Command == AppCommandKind::ShowDefault && devices.Command == AppCommandKind::ListDevices,
          "device listing must refresh once and each explicit query must identify its own result");
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

void TestPickerBusyStateComesFromCommandAdmission() {
    for (auto operation : {AppCommandKind::Connect, AppCommandKind::ReconnectAll, AppCommandKind::DisconnectAll}) {
        AppFixture fixture;
        (void)fixture.Settings->RememberDevice(L"target", L"Target");
        if (operation != AppCommandKind::Connect) apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
        const auto before = fixture.Controller.Snapshot().Generation;
        const auto context = AppCommandContext::Detached();
        auto result = operation == AppCommandKind::Connect
                          ? fixture.Controller.Connect(*DeviceSelector::ById(L"target"), context)
                      : operation == AppCommandKind::ReconnectAll ? fixture.Controller.ReconnectAll(context)
                                                                  : fixture.Controller.DisconnectAll(context);
        const auto snapshot = fixture.Controller.Snapshot();
        const auto view = BuildDevicePickerViewState(snapshot, L"Private device");
        Check(result.Succeeded() && snapshot.Generation != before && view.Items.size() == 1 &&
                  view.Items.front().IsBusy,
              "command admission must publish busy state before the picker renders, including bulk commands");
        if (operation == AppCommandKind::Connect) {
            fixture.Devices->ConnectionAccess->LastConnection->CompleteStart(
                apc::device::DeviceConnectionResult::Failed);
            const auto closing = BuildDevicePickerViewState(fixture.Controller.Snapshot(), L"Private device");
            Check(closing.Items.size() == 1 && closing.Items.front().IsBusy,
                  "a failed connection stays busy until native cleanup completes");
            apc::tests::device::CompleteCloseAndCooldown(*fixture.Devices,
                                                         fixture.Devices->ConnectionAccess->LastConnection);
            const auto finished = BuildDevicePickerViewState(fixture.Controller.Snapshot(), L"Private device");
            Check(finished.Items.size() == 1 && !finished.Items.front().IsBusy &&
                      finished.Generation != view.Generation,
                  "completed failure cleanup must clear picker busy state without a UI expiry timer");
        }
    }
}

void TestQueriesReconcileAllProjectedFieldsAfterOwnerChanges() {
    for (auto query : std::array{&AppController::Status,
                                 &AppController::ListDevices,
                                 &AppController::ListAliases,
                                 &AppController::ShowDefault}) {
        AppFixture fixture;
        (void)fixture.Settings->RememberDevice(L"target", L"Target");
        (void)fixture.Settings->SetDefaultDevice(L"target");
        apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
        auto const before = fixture.Controller.Snapshot();
        bool changed = false;
        fixture.Presentation->BeforeResourceRead = [&] {
            if (std::exchange(changed, true)) return;
            (void)fixture.Settings->SetDeviceAlias(L"target", L"Updated");
            (void)fixture.Service->Disconnect(L"target");
            apc::tests::device::CompleteCloseAndCooldown(*fixture.Devices,
                                                         fixture.Devices->ConnectionAccess->LastConnection);
        };
        auto result = (fixture.Controller.*query)({});
        Check(result.Succeeded() && result.Snapshot && result.Devices == result.Snapshot->Devices &&
                  result.DefaultDevice == result.Snapshot->DefaultDevice && result.Devices.size() == 1 &&
                  result.Devices.front().DisplayName == L"Updated" && !result.Devices.front().IsConnected &&
                  !result.DefaultDevice->IsConnected && result.Snapshot->Generation > before.Generation &&
                  result.Snapshot->SettingsRevision == fixture.Settings->Snapshot().Revision &&
                  result.Snapshot->DeviceGeneration == fixture.Service->Snapshot().Generation,
              "every query must derive all projected fields from the same validated owner capture");
    }
}

void TestSnapshotGenerationTracksOwnersAndPickerWithoutCommands() {
    AppFixture fixture;
    auto previous = fixture.Controller.Snapshot();
    (void)fixture.Service->Start();
    fixture.Devices->WatcherAccess->LastWatcher->Add(L"target", L"Target");
    auto inventory = fixture.Controller.Snapshot();
    Check(inventory.Generation > previous.Generation && inventory.DeviceGeneration > previous.DeviceGeneration &&
              inventory.Devices.size() == 1 && inventory.Tray.Generation == inventory.Generation,
          "owner inventory changes must advance application and tray generations without a controller command");
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
    auto connected = fixture.Controller.Snapshot();
    Check(connected.Generation > inventory.Generation && connected.Devices.front().IsConnected &&
              fixture.Controller.Snapshot().Generation == connected.Generation,
          "a new session state must advance generation once and stable rereads must retain it");
    ++fixture.Presentation->OpenedGeneration;
    auto picker = fixture.Controller.Snapshot();
    Check(picker.Generation > connected.Generation && picker.Tray.DevicePickerOpenedGeneration == 1 &&
              fixture.Controller.Snapshot().Generation == picker.Generation,
          "a later picker acknowledgement must advance generation once even after the action returned");
}

void TestUnstableQueryCannotReportSuccessWithPartialState() {
    AppFixture fixture;
    bool privacy = false;
    fixture.Presentation->BeforeResourceRead = [&] {
        privacy = !privacy;
        (void)fixture.Settings->SetPrivacyModeEnabled(privacy);
    };
    auto result = fixture.Controller.Status({});
    Check(result.Code == AppResultCode::Unavailable && !result.Snapshot && result.Devices.empty(),
          "continuous source changes must fail the query instead of returning mixed fields or a success code");
}

void TestOlderCaptureCannotReuseANewerGenerationForStaleState() {
    AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"target", L"Target");
    (void)fixture.Controller.Snapshot();
    std::binary_semaphore entered(0), release(0);
    std::atomic_bool first = true;
    fixture.Presentation->BeforeResourceRead = [&] {
        if (!first.exchange(false)) return;
        entered.release();
        release.acquire();
    };
    auto older = std::async(std::launch::async, [&] { return fixture.Controller.Snapshot(); });
    entered.acquire();
    (void)fixture.Settings->SetDeviceAlias(L"target", L"Current");
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
    auto newer = fixture.Controller.Snapshot();
    release.release();
    auto completed = older.get();
    Check(completed.IsRunning && completed.Generation == newer.Generation &&
              completed.DeviceGeneration == newer.DeviceGeneration &&
              completed.SettingsRevision == newer.SettingsRevision && completed.Devices == newer.Devices &&
              completed.Devices.front().DisplayName == L"Current" && completed.Devices.front().IsConnected,
          "a delayed reader must recapture current owners instead of stamping stale values with a newer generation");
}

void TestSettingsActionsShareTheStoreAndDevicePolicy() {
    AppFixture fixture;
    (void)fixture.Service->Start();
    fixture.Devices->WatcherAccess->LastWatcher->Add(L"target", L"Target");
    Check(fixture.Controller.SetDeviceConnectOnStartup(L"target", true).IsApplied() &&
              fixture.Controller.SetDeviceReconnectOnConnectionLoss(L"target", true).IsApplied(),
          "per-device actions must remember an observed device and commit its settings through the store");
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"target");
    auto session = fixture.Service->Snapshot().Sessions.front();
    Check(session.IsReconnectEnabled, "a connection must inherit its committed per-device reconnect policy");
    Check(fixture.Controller.SetDeviceReconnectOnConnectionLoss(L"target", false).IsApplied() &&
              !fixture.Service->Snapshot().Sessions.front().IsReconnectEnabled,
          "disabling the persisted device policy must reach the serialized session owner");
    Check(fixture.Controller.SetGlobalReconnectOnConnectionLoss(true).IsApplied() &&
              fixture.Service->Snapshot().Sessions.front().IsReconnectEnabled,
          "global reconnect must update existing sessions from the same revisioned policy");
    Check(fixture.Controller.SetGlobalConnectOnStartup(true).IsApplied() &&
              fixture.Controller.SetShowNotifications(false).IsApplied() &&
              fixture.Controller.SetPrivacyMode(true).IsApplied() &&
              fixture.Controller.SetSystemBackdropEffects(false).IsApplied() &&
              fixture.Controller.SetLanguage(L"de").IsApplied(),
          "general settings must expose committed mutation results through the application endpoint");
    auto snapshot = fixture.Controller.Snapshot();
    Check(snapshot.Settings == fixture.Settings->Snapshot().Data &&
              snapshot.SettingsRevision == fixture.Settings->Snapshot().Revision,
          "the application snapshot must contain the settings value from its validated capture");
    auto const revision = fixture.Settings->Snapshot().Revision;
    fixture.Controller.Shutdown();
    Check(fixture.Controller.SetShowNotifications(true).Status == SettingsMutationStatus::Rejected &&
              fixture.Controller.ForgetDevice(L"target").Status == SettingsMutationStatus::Rejected &&
              fixture.Settings->Snapshot().Revision == revision,
          "settings and device settings actions must reject admission after application shutdown");
    (void)fixture.Settings->SetGlobalReconnectOnConnectionLoss(false);
    Check(fixture.Service->Snapshot().Sessions.front().IsReconnectEnabled,
          "a detached source notification must not reconfigure devices after controller shutdown");
}

} // namespace

int RunAppControllerUseCasesTests() {
    TestSettingsActionsShareTheStoreAndDevicePolicy();
    TestQueriesReconcileAllProjectedFieldsAfterOwnerChanges();
    TestSnapshotGenerationTracksOwnersAndPickerWithoutCommands();
    TestUnstableQueryCannotReportSuccessWithPartialState();
    TestOlderCaptureCannotReuseANewerGenerationForStaleState();
    TestExplicitSettingsInputContracts();
    TestQueriesChooseTheirRequiredInputs();
    TestTargetResolutionAndDefaultModes();
    TestSettingsResultsReadTheCommittedOwner();
    TestRefreshCancellationCannotAdmitMutation();
    TestRefreshFailureRetainsKnownTargets();
    TestRefreshReadsCurrentSessionsAfterEnumeration();
    TestOpaqueExternalIdentityAndIdempotency();
    TestDetachedToggleRespectsOwnedBusyState();
    TestPickerBusyStateComesFromCommandAdmission();
    return g_failures;
}
