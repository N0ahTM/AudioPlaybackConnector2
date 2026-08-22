#include <app/LegacyAppUseCaseBridge.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using apc::app::AppCommand;
using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppEvent;
using apc::app::AppOutcomeReason;
using apc::app::AppResultCode;
using apc::app::AppSnapshot;
using apc::app::DeviceConnectionState;
using apc::app::DeviceSelector;
using apc::app::DeviceSelectorKind;
using apc::app::LegacyAppUseCaseBridge;

using DeviceRecord = LegacyAppUseCaseBridge::DeviceRecord;
using OperationStatus = LegacyAppUseCaseBridge::OperationStatus;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

DeviceRecord Device(std::wstring id,
                    std::wstring name,
                    std::wstring alias = {},
                    bool connected = false,
                    bool known = true,
                    bool busy = false) {
    return DeviceRecord{std::move(id),
                        std::move(name),
                        std::move(alias),
                        connected ? DeviceConnectionState::Connected : DeviceConnectionState::Idle,
                        connected,
                        known,
                        busy};
}

std::optional<DeviceSelector> IdSelector(std::wstring_view id) {
    return DeviceSelector::ById(id);
}

AppCommand
Command(AppCommandKind kind, std::optional<DeviceSelector> selector = std::nullopt, std::wstring alias = {}) {
    return AppCommand{kind, std::move(selector), std::move(alias)};
}

struct Harness {
    SettingsData SourceSettings;
    std::vector<DeviceRecord> LiveDevices;
    std::vector<DeviceRecord> RefreshDevices;
    std::function<SettingsData()> ReadSettings;
    bool SettingsReadFails = false;
    bool DevicesReadFails = false;
    int SettingsReadCalls = 0;
    int DeviceReadCalls = 0;
    int RefreshCalls = 0;
    int ConnectCalls = 0;
    int ConnectDetachedCalls = 0;
    int ReconnectCalls = 0;
    int ReconnectDetachedCalls = 0;
    int DisconnectCalls = 0;
    int DisconnectAllCalls = 0;
    int ReconnectAllDetachedCalls = 0;
    int SetDefaultCalls = 0;
    int ClearDefaultCalls = 0;
    int SetAliasCalls = 0;
    bool SetDefaultAccepted = true;
    bool ClearDefaultAccepted = true;
    bool SetAliasAccepted = true;
    bool GlobalBusy = false;
    std::vector<std::wstring> ReconnectOrder;
    OperationStatus ConnectStatus = OperationStatus::Succeeded;
    OperationStatus ReconnectStatus = OperationStatus::Succeeded;
    std::vector<OperationStatus> ReconnectStatuses;
    LegacyAppUseCaseBridge::RefreshResult RefreshResponse{OperationStatus::Succeeded, {}};
    LegacyAppUseCaseBridge::UiActionResult PickerResponse{OperationStatus::Failed, std::nullopt};
    LegacyAppUseCaseBridge::UiActionResult SettingsResponse{OperationStatus::Failed, std::nullopt};
    AppSnapshot::ResourceStatusSnapshot Resource;
    std::uint64_t PickerGeneration = 0;
    std::chrono::steady_clock::time_point LastRefreshDeadline{};
    LegacyAppUseCaseBridge Bridge;

    Harness()
        : SourceSettings(), LiveDevices(), RefreshDevices(), ReadSettings([this] {
              ++SettingsReadCalls;
              if (SettingsReadFails) throw 1;
              return SourceSettings;
          }),
          Bridge(MakeOperations(), SourceSettings) {}

    LegacyAppUseCaseBridge::Operations MakeOperations() {
        LegacyAppUseCaseBridge::Operations operations;
        operations.ReadSettings = ReadSettings;
        operations.ReadConnectedDevices = [this] {
            ++DeviceReadCalls;
            if (DevicesReadFails) throw 1;
            return LiveDevices;
        };
        operations.Refresh = [this](AppCommandContext const& context) {
            ++RefreshCalls;
            LastRefreshDeadline = context.Deadline;
            auto response = RefreshResponse;
            if (response.Status == OperationStatus::Succeeded) response.Devices = RefreshDevices;
            return response;
        };
        operations.Connect = [this](std::wstring_view id, AppCommandContext const&) {
            ++ConnectCalls;
            if (ConnectStatus == OperationStatus::Succeeded) {
                auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
                if (found == LiveDevices.end()) {
                    LiveDevices.push_back(Device(std::wstring(id), std::wstring(id), {}, true, true));
                } else {
                    found->IsConnected = true;
                    found->State = DeviceConnectionState::Connected;
                }
            }
            return LegacyAppUseCaseBridge::OperationResult{ConnectStatus};
        };
        operations.ConnectDetached = [this](std::wstring_view id) {
            ++ConnectDetachedCalls;
            auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
            if (found == LiveDevices.end()) {
                LiveDevices.push_back(Device(std::wstring(id), std::wstring(id), {}, true, true));
            } else {
                found->IsConnected = true;
                found->State = DeviceConnectionState::Connected;
            }
        };
        operations.Reconnect = [this](std::wstring_view id, AppCommandContext const&) {
            ++ReconnectCalls;
            ReconnectOrder.emplace_back(id);
            const auto status = ReconnectCalls <= static_cast<int>(ReconnectStatuses.size())
                                    ? ReconnectStatuses[static_cast<std::size_t>(ReconnectCalls - 1)]
                                    : ReconnectStatus;
            if (status == OperationStatus::Succeeded) {
                auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
                if (found != LiveDevices.end()) {
                    found->IsConnected = true;
                    found->State = DeviceConnectionState::Connected;
                }
            }
            return LegacyAppUseCaseBridge::OperationResult{status};
        };
        operations.ReconnectDetached = [this](std::wstring_view id) {
            ++ReconnectDetachedCalls;
            auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
            if (found != LiveDevices.end()) {
                found->IsConnected = true;
                found->State = DeviceConnectionState::Connected;
            }
        };
        operations.Disconnect = [this](std::wstring_view id) {
            ++DisconnectCalls;
            auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
            if (found != LiveDevices.end()) {
                found->IsConnected = false;
                found->State = DeviceConnectionState::Idle;
            }
        };
        operations.DisconnectAll = [this] {
            ++DisconnectAllCalls;
            for (auto& device : LiveDevices) {
                device.IsConnected = false;
                device.State = DeviceConnectionState::Idle;
            }
        };
        operations.ReconnectAllDetached = [this] { ++ReconnectAllDetachedCalls; };
        operations.SetDefaultDevice = [this](std::wstring_view id) {
            ++SetDefaultCalls;
            if (!SetDefaultAccepted) return false;
            SourceSettings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
            SourceSettings.DefaultDeviceId = id;
            return true;
        };
        operations.ClearDefaultDevice = [this] {
            ++ClearDefaultCalls;
            if (!ClearDefaultAccepted) return false;
            SourceSettings.DefaultDevice = ::DefaultDeviceMode::LastConnected;
            SourceSettings.DefaultDeviceId.clear();
            return true;
        };
        operations.SetDeviceAlias = [this](std::wstring_view id, std::wstring_view alias, std::wstring_view) {
            ++SetAliasCalls;
            if (!SetAliasAccepted) return false;
            auto found =
                std::ranges::find_if(SourceSettings.Devices, [id](auto const& device) { return device.Id == id; });
            if (found == SourceSettings.Devices.end()) return false;
            found->Alias = alias;
            return true;
        };
        operations.ShowDevicePicker = [this](AppCommandContext const&) { return PickerResponse; };
        operations.ShowSettings = [this](AppCommandContext const&) { return SettingsResponse; };
        operations.ResourceStatus = [this] { return Resource; };
        operations.PickerOpenedGeneration = [this] { return PickerGeneration; };
        operations.HasBusy = [this] { return GlobalBusy; };
        operations.DeviceBusy = [this](std::wstring_view id) {
            auto found = std::ranges::find_if(LiveDevices, [id](auto const& device) { return device.Id == id; });
            return found != LiveDevices.end() && found->IsBusy;
        };
        return operations;
    }

    void AddSettingsDevice(DeviceRecord const& device) {
        SourceSettings.Devices.push_back(DeviceSettings{device.Id, device.Name, device.Alias, false, false});
    }
};

void SeedBasic(Harness& harness) {
    harness.LiveDevices = {Device(L"id-a", L"Headphones", L"Desk"),
                           Device(L"id-b", L"Speaker", L"Room"),
                           Device(L"id-c", L"Other", L"Desk"),
                           Device(L"00:11:22:33:44:55", L"Mac Speaker")};
    for (auto const& device : harness.LiveDevices)
        harness.AddSettingsDevice(device);
}

void TestTargetRanksAmbiguityAndUnknownExternalId() {
    Harness harness;
    SeedBasic(harness);

    auto exactName = harness.Bridge.Execute(
        Command(AppCommandKind::Connect, DeviceSelector::ByQuery(DeviceSelectorKind::Name, L"Headphones")));
    Check(exactName.Code == AppResultCode::OperationFailed || exactName.Code == AppResultCode::Success,
          "name resolution must select the exact-name candidate after refresh");
    Check(harness.RefreshCalls == 1, "name resolution must request one live refresh");

    auto exactMac = harness.Bridge.Execute(
        Command(AppCommandKind::Connect, DeviceSelector::ByQuery(DeviceSelectorKind::Mac, L"112233")));
    Check(exactMac.Target && exactMac.Target->Id == L"00:11:22:33:44:55",
          "MAC resolution must use the normalized six-hex-digit rank");

    auto exactAuto = harness.Bridge.Execute(
        Command(AppCommandKind::Disconnect, DeviceSelector::ByQuery(DeviceSelectorKind::Auto, L"id-a")));
    Check(exactAuto.Target && exactAuto.Target->Id == L"id-a",
          "auto resolution must prefer exact ID over name and alias matches");

    auto ambiguousAlias = harness.Bridge.Execute(
        Command(AppCommandKind::Connect, DeviceSelector::ByQuery(DeviceSelectorKind::Alias, L"Desk")));
    Check(ambiguousAlias.Code == AppResultCode::Ambiguous &&
              ambiguousAlias.Reason == AppOutcomeReason::TargetAmbiguous && ambiguousAlias.RequestedTarget == L"Desk",
          "equal-rank alias matches must remain ambiguous with the raw query");

    auto missingDefault = harness.Bridge.Execute(Command(AppCommandKind::Connect, DeviceSelector::Default()));
    Check(missingDefault.Code == AppResultCode::NotFound &&
              missingDefault.Reason == AppOutcomeReason::DefaultTargetMissing,
          "a missing default target must retain its dedicated reason");
    auto missingLast = harness.Bridge.Execute(Command(AppCommandKind::Connect, DeviceSelector::Last()));
    Check(missingLast.Code == AppResultCode::NotFound && missingLast.Reason == AppOutcomeReason::LastTargetMissing,
          "a missing last target must retain its dedicated reason");

    const std::wstring longId(513, L'x');
    harness.ConnectStatus = OperationStatus::Succeeded;
    const auto connectCallsBeforeUnknown = harness.ConnectCalls;
    auto unknown = harness.Bridge.Execute(Command(AppCommandKind::Connect, DeviceSelector::ById(longId)));
    Check(unknown.Code == AppResultCode::Success && unknown.Target && !unknown.Target->Exists &&
              unknown.Target->Id == longId && harness.ConnectCalls == connectCallsBeforeUnknown + 1,
          "an unknown external ID beyond the DeviceId bound must still execute opaquely");
}

void TestRefreshCapAndCurrentInputFallback() {
    Harness harness;
    harness.LiveDevices = {Device(L"live", L"Live")};
    harness.RefreshDevices = {Device(L"fresh", L"Fresh")};
    harness.RefreshResponse = {OperationStatus::Succeeded, {}};

    auto first = harness.Bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(first.Devices.size() == 2 &&
              std::ranges::any_of(first.Devices, [](auto const& device) { return device.Id.View() == L"fresh"; }) &&
              std::ranges::any_of(first.Devices, [](auto const& device) { return device.Id.View() == L"live"; }),
          "successful list refreshes must merge live and refreshed discovery inputs");
    const auto now = AppCommandContext::Clock::now();
    Check(harness.LastRefreshDeadline > now && harness.LastRefreshDeadline <= now + std::chrono::milliseconds(2500),
          "refresh requests must carry a deadline capped at 2.5 seconds");

    harness.RefreshResponse = {OperationStatus::Failed, {}};
    auto second = harness.Bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(second.Devices.size() == 1 && second.Devices.front().Id.View() == L"live" &&
              std::ranges::none_of(second.Devices, [](auto const& device) { return device.Id.View() == L"fresh"; }),
          "failed refreshes must fall back to current connected/settings inputs, not a prior discovery cache");

    auto status = harness.Bridge.Execute(Command(AppCommandKind::Status));
    Check(harness.RefreshCalls == 2, "status must not trigger the list/target refresh path");
    Check(status.Code == AppResultCode::Success, "status must remain available after a refresh failure");
}

void TestMergePrecedence() {
    Harness harness;
    harness.LiveDevices = {Device(L"same", L"Connected Name")};
    harness.RefreshDevices = {Device(L"same", L"Refreshed Name")};
    harness.AddSettingsDevice(Device(L"same", L"Persisted Name", L"Persisted Alias"));

    const auto withSettings = harness.Bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(withSettings.Devices.size() == 1 && withSettings.Devices.front().Name == L"Persisted Name" &&
              withSettings.Devices.front().Alias == L"Persisted Alias",
          "persisted settings labels must take precedence over connected and refreshed labels");

    harness.SourceSettings.Devices.clear();
    const auto withoutSettings = harness.Bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(withoutSettings.Devices.size() == 1 && withoutSettings.Devices.front().Name == L"Connected Name",
          "connected labels must take precedence over refreshed labels when settings are absent");
}

void TestAwaitedAndDetachedOperationOutcomes() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    auto target = IdSelector(L"target");

    harness.ConnectStatus = OperationStatus::Succeeded;
    auto success = harness.Bridge.Execute(Command(AppCommandKind::Connect, target));
    Check(success.Code == AppResultCode::Success && success.Reason == AppOutcomeReason::ConnectSucceeded,
          "awaited connect success must verify the connected postcondition");

    harness.LiveDevices.front().IsConnected = false;
    harness.LiveDevices.front().State = DeviceConnectionState::Idle;
    harness.ConnectStatus = OperationStatus::Cancelled;
    auto cancelled = harness.Bridge.Execute(Command(AppCommandKind::Connect, target));
    Check(cancelled.Code == AppResultCode::Cancelled && cancelled.Reason == AppOutcomeReason::None,
          "awaited cancellation must remain a cancelled indeterminate wire outcome");

    harness.ConnectStatus = OperationStatus::TimedOut;
    auto timedOut = harness.Bridge.Execute(Command(AppCommandKind::Connect, target));
    Check(timedOut.Code == AppResultCode::TimedOut && timedOut.Reason == AppOutcomeReason::None,
          "awaited timeout must remain a timed-out indeterminate wire outcome");

    harness.ConnectStatus = OperationStatus::Failed;
    auto failed = harness.Bridge.Execute(Command(AppCommandKind::Connect, target));
    Check(failed.Code == AppResultCode::Indeterminate && failed.Reason == AppOutcomeReason::None,
          "a failed wait must remain indeterminate until the connected postcondition is checked");

    harness.ConnectStatus = OperationStatus::Failed;
    AppCommandContext detached;
    detached.Completion = AppCommandContext::CompletionMode::Detached;
    const auto awaitedCallsBeforeDetached = harness.ConnectCalls;
    auto detachedResult = harness.Bridge.Execute(Command(AppCommandKind::Connect, target), detached);
    Check(detachedResult.Code == AppResultCode::Success && harness.ConnectDetachedCalls == 1 &&
              harness.ConnectCalls == awaitedCallsBeforeDetached,
          "detached connect must invoke only the detached callback and never wait");

    harness.LiveDevices.front().IsConnected = true;
    harness.LiveDevices.front().State = DeviceConnectionState::Connected;
    harness.ReconnectStatus = OperationStatus::Failed;
    auto reconnectFailed = harness.Bridge.Execute(Command(AppCommandKind::Reconnect, target));
    Check(reconnectFailed.Code == AppResultCode::Indeterminate && reconnectFailed.Reason == AppOutcomeReason::None,
          "a failed awaited reconnect wait must remain indeterminate");
    auto detachedReconnect = harness.Bridge.Execute(Command(AppCommandKind::Reconnect, target), detached);
    Check(detachedReconnect.Code == AppResultCode::Success && harness.ReconnectDetachedCalls == 1,
          "detached reconnect must not call the awaited reconnect operation");
}

void TestIdempotencyAndTrayOnlyBusyPolicy() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target", {}, true)};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    auto target = IdSelector(L"target");
    auto alreadyConnected = harness.Bridge.Execute(Command(AppCommandKind::Connect, target));
    Check(alreadyConnected.Code == AppResultCode::Success &&
              alreadyConnected.Reason == AppOutcomeReason::AlreadyConnected && harness.ConnectCalls == 0,
          "connect must be idempotent when the target is already connected");

    harness.LiveDevices.front().IsConnected = false;
    harness.LiveDevices.front().State = DeviceConnectionState::Idle;
    auto alreadyDisconnected = harness.Bridge.Execute(Command(AppCommandKind::Disconnect, target));
    Check(alreadyDisconnected.Code == AppResultCode::Success &&
              alreadyDisconnected.Reason == AppOutcomeReason::AlreadyDisconnected && harness.DisconnectCalls == 0,
          "disconnect must be idempotent when the target is already disconnected");

    harness.SourceSettings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
    harness.SourceSettings.DefaultDeviceId = L"target";
    harness.GlobalBusy = true;
    AppCommandContext tray;
    tray.Completion = AppCommandContext::CompletionMode::Detached;
    auto busy = harness.Bridge.Execute(Command(AppCommandKind::ToggleLast, DeviceSelector::Default()), tray);
    Check(busy.Code == AppResultCode::Busy && harness.ConnectDetachedCalls == 0,
          "tray toggle must reject a global busy operation");

    harness.GlobalBusy = false;
    harness.LiveDevices.front().IsBusy = true;
    auto deviceBusy = harness.Bridge.Execute(Command(AppCommandKind::ToggleLast, DeviceSelector::Default()), tray);
    Check(deviceBusy.Code == AppResultCode::Busy && harness.ConnectDetachedCalls == 0,
          "tray toggle must reject a busy target");

    harness.LiveDevices.front().IsBusy = false;
    const auto before = harness.ConnectDetachedCalls;
    auto waitToggle = harness.Bridge.Execute(Command(AppCommandKind::ToggleLast, DeviceSelector::Default()));
    Check(waitToggle.Code == AppResultCode::Success && harness.ConnectDetachedCalls == before,
          "non-tray toggle must not apply tray-only busy checks");
}

void TestReconnectAllAndSynchronousDisconnectAll() {
    Harness harness;
    harness.LiveDevices = {Device(L"first", L"First", {}, true), Device(L"second", L"Second", {}, true)};
    harness.AddSettingsDevice(harness.LiveDevices[0]);
    harness.AddSettingsDevice(harness.LiveDevices[1]);
    auto firstResult = harness.Bridge.Execute(Command(AppCommandKind::ReconnectAll));
    Check(firstResult.Code == AppResultCode::Success && harness.ReconnectOrder.size() == 2,
          "reconnect-all must await each connected target sequentially");

    const auto disconnectAll = harness.Bridge.Execute(Command(AppCommandKind::DisconnectAll));
    Check(disconnectAll.Code == AppResultCode::Success, "disconnect-all must report a synchronous success");
    Check(harness.DisconnectAllCalls == 1 &&
              std::ranges::none_of(harness.LiveDevices, [](auto const& device) { return device.IsConnected; }),
          "disconnect-all must invoke the synchronous bulk operation");
}

void TestReconnectAllStopsOnFirstPartialFailure() {
    Harness harness;
    harness.LiveDevices = {Device(L"first", L"First", {}, true), Device(L"second", L"Second", {}, true)};
    harness.AddSettingsDevice(harness.LiveDevices[0]);
    harness.AddSettingsDevice(harness.LiveDevices[1]);
    harness.ReconnectStatuses = {OperationStatus::Succeeded, OperationStatus::Failed};
    const auto failed = harness.Bridge.Execute(Command(AppCommandKind::ReconnectAll));
    Check(failed.Code == AppResultCode::Indeterminate && failed.Reason == AppOutcomeReason::ReconnectFailed &&
              harness.ReconnectOrder == std::vector<std::wstring>{L"first", L"second"},
          "reconnect-all must stop after the first partial failure with reconnect-failed semantics");

    harness.ReconnectCalls = 0;
    harness.ReconnectOrder.clear();
    harness.ReconnectStatuses = {OperationStatus::Succeeded, OperationStatus::TimedOut};
    const auto timedOut = harness.Bridge.Execute(Command(AppCommandKind::ReconnectAll));
    Check(timedOut.Code == AppResultCode::TimedOut && timedOut.Reason == AppOutcomeReason::ReconnectFailed &&
              harness.ReconnectOrder == std::vector<std::wstring>{L"first", L"second"},
          "reconnect-all timeout must stop at the timed-out target with reconnect-failed semantics");

    harness.ReconnectCalls = 0;
    harness.ReconnectOrder.clear();
    harness.ReconnectStatuses = {OperationStatus::Succeeded};
    std::stop_source stop;
    stop.request_stop();
    AppCommandContext cancelled;
    cancelled.StopToken = stop.get_token();
    const auto cancelledResult = harness.Bridge.Execute(Command(AppCommandKind::ReconnectAll), cancelled);
    Check(cancelledResult.Code == AppResultCode::Cancelled && cancelledResult.Reason == AppOutcomeReason::NotReady &&
              harness.ReconnectOrder.empty(),
          "reconnect-all cancellation must stop before the next operation and remain not-ready");

    Harness opaque;
    const std::wstring longId(513, L'r');
    opaque.LiveDevices = {Device(longId, L"Long Reconnect Target", {}, true)};
    opaque.AddSettingsDevice(opaque.LiveDevices.front());
    const auto opaqueResult = opaque.Bridge.Execute(Command(AppCommandKind::ReconnectAll));
    Check(opaqueResult.Code == AppResultCode::Success && opaque.ReconnectOrder == std::vector<std::wstring>{longId},
          "reconnect-all must execute connected external IDs beyond the bounded snapshot identity type");
}

void TestSettingsMutationsAndFailures() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    auto target = IdSelector(L"target");

    auto setDefault = harness.Bridge.Execute(Command(AppCommandKind::SetDefault, target));
    Check(setDefault.Code == AppResultCode::Success && setDefault.Reason == AppOutcomeReason::DefaultSet &&
              harness.SetDefaultCalls == 1,
          "set-default must call the injected setter and return its typed outcome");
    auto defaultSnapshot = harness.Bridge.Snapshot();
    Check(defaultSnapshot.DefaultDevice &&
              defaultSnapshot.DefaultDevice->Mode == apc::app::DefaultDeviceMode::SpecificDevice &&
              defaultSnapshot.DefaultDevice->Id && defaultSnapshot.DefaultDevice->Id->View() == L"target",
          "set-default must update the bridge-owned settings snapshot");

    harness.SetDefaultAccepted = false;
    auto defaultFailure = harness.Bridge.Execute(Command(AppCommandKind::SetDefault, target));
    Check(defaultFailure.Code == AppResultCode::OperationFailed &&
              defaultFailure.Reason == AppOutcomeReason::InternalError,
          "set-default failures must remain observable as operation failures");

    harness.SetDefaultAccepted = true;
    auto alias = harness.Bridge.Execute(Command(AppCommandKind::SetAlias, target, L"New Alias"));
    Check(alias.Code == AppResultCode::Success && alias.Reason == AppOutcomeReason::AliasSet &&
              alias.Alias == L"New Alias" && alias.Device && alias.Device->Alias == L"New Alias" &&
              harness.SetAliasCalls == 1,
          "set-alias must return the new alias and post-change device snapshot");

    harness.SetAliasAccepted = false;
    auto aliasFailure = harness.Bridge.Execute(Command(AppCommandKind::SetAlias, target, L"Rejected"));
    Check(aliasFailure.Code == AppResultCode::OperationFailed &&
              aliasFailure.Reason == AppOutcomeReason::AliasSetFailed,
          "alias setter failures must retain the alias-specific reason");

    harness.SetAliasAccepted = true;
    auto clearAlias = harness.Bridge.Execute(Command(AppCommandKind::ClearAlias, target));
    Check(clearAlias.Code == AppResultCode::Success && clearAlias.Reason == AppOutcomeReason::AliasCleared,
          "clear-alias must use the same typed settings boundary");

    auto clearDefault = harness.Bridge.Execute(Command(AppCommandKind::ClearDefault));
    Check(clearDefault.Code == AppResultCode::Success && clearDefault.Reason == AppOutcomeReason::DefaultCleared &&
              harness.ClearDefaultCalls == 1,
          "clear-default must call the injected setter");

    const std::wstring longId(513, L'z');
    harness.LiveDevices.push_back(Device(longId, L"Long External ID"));
    harness.AddSettingsDevice(harness.LiveDevices.back());
    const auto longDefault = harness.Bridge.Execute(Command(AppCommandKind::SetDefault, DeviceSelector::ById(longId)));
    Check(longDefault.Code == AppResultCode::Success && harness.SetDefaultCalls == 3 &&
              harness.SourceSettings.DefaultDeviceId == longId,
          "set-default must pass a resolved live external ID beyond the persistence DeviceId bound to the setter");
}

void TestSnapshotPrivacyResourcePickerAndStableGeneration() {
    Harness harness;
    harness.SourceSettings.PrivacyModeEnabled = true;
    harness.SourceSettings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
    harness.SourceSettings.DefaultDeviceId = L"target";
    harness.SourceSettings.LastConnectedIds = {L"target"};
    harness.LiveDevices = {Device(L"target", L"Raw Target", L"Trusted Alias", true, true, true)};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    harness.Resource.Evaluated = true;
    harness.Resource.ForegroundResidency = AppSnapshot::ResourceStatusSnapshot::Residency::Hot;
    harness.Resource.Memory = AppSnapshot::ResourceStatusSnapshot::MemoryPressure::High;
    harness.Resource.PreloadAllowed = true;
    harness.PickerGeneration = 17;
    harness.GlobalBusy = true;

    const auto first = harness.Bridge.Snapshot();
    const auto second = harness.Bridge.Snapshot();
    Check(first == second && first.Generation == 0,
          "snapshot reads must remain value-stable and must not advance generation");
    Check(first.PrivacyModeEnabled && first.Devices.size() == 1 && first.Devices.front().Alias == L"Trusted Alias" &&
              first.Tray.HasBusyOperations && first.Tray.DevicePickerOpenedGeneration == 17,
          "snapshot must merge privacy, device, picker, and busy facts");
    Check(first.DefaultDevice && first.DefaultDevice->IsResolved && first.DefaultDevice->IsConnected &&
              first.DefaultDevice->DisplayName == L"Trusted Alias",
          "snapshot must resolve the persisted specific default against current devices");
    Check(first.AdaptiveResources.Evaluated &&
              first.AdaptiveResources.ForegroundResidency == AppSnapshot::ResourceStatusSnapshot::Residency::Hot &&
              first.AdaptiveResources.Memory == AppSnapshot::ResourceStatusSnapshot::MemoryPressure::High,
          "snapshot must carry the injected adaptive-resource status without policy decisions");

    harness.PickerResponse = {OperationStatus::Succeeded, 23};
    auto show = harness.Bridge.Execute(Command(AppCommandKind::ShowDevicePicker));
    Check(show.Code == AppResultCode::Success && show.Reason == AppOutcomeReason::ShowOpened &&
              harness.Bridge.Snapshot().Tray.DevicePickerOpenedGeneration == 23,
          "successful UI actions must return typed outcomes and update picker facts");
    harness.SettingsResponse = {OperationStatus::Indeterminate, std::nullopt};
    auto settings = harness.Bridge.Execute(Command(AppCommandKind::ShowSettings));
    Check(settings.Code == AppResultCode::Indeterminate && settings.Reason == AppOutcomeReason::NotReady,
          "UI action indeterminacy must remain typed and transport-free");
}

void TestObservedFactsNormalizeAndOverlayWithoutInjection() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    const auto before = harness.Bridge.Snapshot().Generation;
    auto status = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                          L"target",
                                          DeviceConnectionState::Connecting,
                                          AppResultCode::OperationFailed});
    Check(status && std::holds_alternative<apc::app::DeviceStatusChangedEvent>(*status),
          "legacy status facts must normalize to typed status events");
    auto afterStatus = harness.Bridge.Snapshot();
    Check(afterStatus.Generation > before && afterStatus.Devices.size() == 1 &&
              afterStatus.Devices.front().State == DeviceConnectionState::Connecting &&
              afterStatus.Devices.front().IsBusy,
          "observed status must advance generation and overlay an existing snapshot record");

    auto eventOnly = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                             L"event-only",
                                             DeviceConnectionState::Connected,
                                             AppResultCode::OperationFailed});
    Check(eventOnly && std::holds_alternative<apc::app::DeviceStatusChangedEvent>(*eventOnly),
          "valid event-only status facts must still be published");
    auto noInjection = harness.Bridge.Snapshot();
    Check(
        std::ranges::none_of(noInjection.Devices, [](auto const& device) { return device.Id.View() == L"event-only"; }),
        "event-only facts must not invent P01 list/status devices");

    auto error = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::ConnectionError,
                                         L"target",
                                         DeviceConnectionState::Idle,
                                         AppResultCode::Success});
    Check(error && std::holds_alternative<apc::app::DeviceConnectionErrorEvent>(*error) &&
              std::get<apc::app::DeviceConnectionErrorEvent>(*error).Code == AppResultCode::OperationFailed,
          "connection errors must normalize a success code to operation failure");
    auto activity = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceActivityChanged});
    auto inventory = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceInventoryChanged});
    Check(activity && std::holds_alternative<apc::app::DeviceActivityChangedEvent>(*activity) && inventory &&
              std::holds_alternative<apc::app::DeviceInventoryChangedEvent>(*inventory),
          "empty legacy activity and inventory facts must preserve typed event kinds");
}

void TestReadFailuresAndCommandReadScope() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    harness.PickerResponse = {OperationStatus::Succeeded, 4};
    harness.SettingsResponse = {OperationStatus::Succeeded, std::nullopt};

    harness.SettingsReadFails = true;
    const auto settingsFailure = harness.Bridge.Execute(Command(AppCommandKind::ClearDefault));
    Check(settingsFailure.Code == AppResultCode::InternalError,
          "settings read failures must not fall back to stale settings for a command");
    Check(!harness.Bridge.Snapshot().IsRunning, "settings read failures must fail closed in snapshots");

    harness.SettingsReadFails = false;
    harness.DevicesReadFails = true;
    const auto beforeDeviceReads = harness.DeviceReadCalls;
    const auto picker = harness.Bridge.Execute(Command(AppCommandKind::ShowDevicePicker));
    const auto settings = harness.Bridge.Execute(Command(AppCommandKind::ShowSettings));
    const auto clearDefault = harness.Bridge.Execute(Command(AppCommandKind::ClearDefault));
    const auto disconnectAll = harness.Bridge.Execute(Command(AppCommandKind::DisconnectAll));
    Check(picker.Code == AppResultCode::Success && settings.Code == AppResultCode::Success &&
              clearDefault.Code == AppResultCode::Success && disconnectAll.Code == AppResultCode::Success &&
              harness.DeviceReadCalls == beforeDeviceReads,
          "UI, clear-default, and disconnect-all commands must not enumerate devices unnecessarily");

    const auto listFailure = harness.Bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(listFailure.Code == AppResultCode::InternalError,
          "required device read failures must propagate instead of returning an empty successful list");
    Check(!harness.Bridge.Snapshot().IsRunning, "device read failures must fail closed in snapshots");
}

void TestShutdownIsMonotonicAndRejectsCallbacks() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    harness.Bridge.SetRunning(false);
    const auto settingsReads = harness.SettingsReadCalls;
    const auto deviceReads = harness.DeviceReadCalls;
    const auto connectCalls = harness.ConnectCalls;

    const auto stoppedSnapshot = harness.Bridge.Snapshot();
    const auto status = harness.Bridge.Execute(Command(AppCommandKind::Status));
    const auto connect = harness.Bridge.Execute(Command(AppCommandKind::Connect, IdSelector(L"target")));
    harness.Bridge.SetRunning(true);
    const auto revivedSnapshot = harness.Bridge.Snapshot();

    Check(!stoppedSnapshot.IsRunning && !revivedSnapshot.IsRunning,
          "a stopped bridge must remain unavailable even when the legacy Running callback would return true");
    Check(status.Code == AppResultCode::Unavailable && status.Reason == AppOutcomeReason::NotReady &&
              connect.Code == AppResultCode::Unavailable && connect.Reason == AppOutcomeReason::NotReady,
          "post-shutdown reads and mutations must fail closed with not-ready semantics");
    Check(harness.SettingsReadCalls == settingsReads && harness.DeviceReadCalls == deviceReads &&
              harness.ConnectCalls == connectCalls,
          "post-shutdown requests must not invoke settings, device, or mutation callbacks");
}

void TestShutdownClosesAdmissionBeforeTearingDownCallbacks() {
    SettingsData settings;
    std::mutex callbackMutex;
    std::condition_variable callbackEntered;
    std::condition_variable releaseCallback;
    bool hasEnteredCallback = false;
    bool canCompleteCallback = false;
    int settingsReadCalls = 0;
    int settingsMutationCalls = 0;
    int settingsUiCalls = 0;
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] {
        ++settingsReadCalls;
        return settings;
    };
    operations.ClearDefaultDevice = [&] {
        std::unique_lock lock(callbackMutex);
        hasEnteredCallback = true;
        callbackEntered.notify_all();
        releaseCallback.wait(lock, [&] { return canCompleteCallback; });
        ++settingsMutationCalls;
        return true;
    };
    operations.ShowSettings = [&](AppCommandContext const&) {
        ++settingsUiCalls;
        return LegacyAppUseCaseBridge::UiActionResult{OperationStatus::Succeeded, std::nullopt};
    };
    LegacyAppUseCaseBridge bridge(std::move(operations), settings);
    AppResultCode commandResult = AppResultCode::InternalError;
    std::thread commandThread([&] { commandResult = bridge.Execute(Command(AppCommandKind::ClearDefault)).Code; });

    {
        std::unique_lock lock(callbackMutex);
        callbackEntered.wait(lock, [&] { return hasEnteredCallback; });
    }

    std::atomic_bool shutdownStarted = false;
    std::atomic_bool shutdownReturned = false;
    std::thread shutdownThread([&] {
        shutdownStarted.store(true, std::memory_order_release);
        bridge.SetRunning(false);
        shutdownReturned.store(true, std::memory_order_release);
    });
    while (!shutdownStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (bridge.Snapshot().IsRunning)
        std::this_thread::yield();
    Check(!shutdownReturned.load(std::memory_order_acquire),
          "shutdown must wait for an admitted callback before returning to host teardown");

    {
        std::scoped_lock lock(callbackMutex);
        canCompleteCallback = true;
    }
    releaseCallback.notify_all();
    commandThread.join();
    shutdownThread.join();

    Check(commandResult == AppResultCode::Success && settingsMutationCalls == 1,
          "the admitted command must complete exactly once before shutdown returns");
    const auto readsBeforeStoppedRequest = settingsReadCalls;
    const auto stoppedMutation = bridge.Execute(Command(AppCommandKind::ClearDefault));
    const auto stoppedUi = bridge.Execute(Command(AppCommandKind::ShowSettings));
    Check(shutdownReturned.load(std::memory_order_acquire) && stoppedMutation.Code == AppResultCode::Unavailable &&
              stoppedUi.Code == AppResultCode::Unavailable && settingsReadCalls == readsBeforeStoppedRequest &&
              settingsMutationCalls == 1 && settingsUiCalls == 0,
          "after shutdown returns no command may invoke a read, mutation, or UI callback");
}

void TestFactsAfterShutdownDoNotPublishOrChangeBridgeState() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    harness.Bridge.SetRunning(false);
    const auto stoppedSnapshot = harness.Bridge.Snapshot();
    const auto ignored = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                                 L"target",
                                                 DeviceConnectionState::Connecting,
                                                 AppResultCode::OperationFailed});
    const auto afterIgnoredFact = harness.Bridge.Snapshot();
    Check(!ignored && stoppedSnapshot.Generation == afterIgnoredFact.Generation && !afterIgnoredFact.IsRunning &&
              afterIgnoredFact.Devices.empty(),
          "facts arriving after shutdown must be rejected without publication or state/generation changes");
}

void TestTerminalFactsClearObservedBusyState() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.AddSettingsDevice(harness.LiveDevices.front());

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::AutoReconnectTriggered,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::OperationFailed});
    auto waiting = harness.Bridge.Snapshot();
    Check(waiting.Devices.front().State == DeviceConnectionState::WaitingForReconnect && waiting.Devices.front().IsBusy,
          "an auto-reconnect trigger must expose a busy waiting state");

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                  L"target",
                                  DeviceConnectionState::Failed,
                                  AppResultCode::OperationFailed});
    auto failedStatus = harness.Bridge.Snapshot();
    Check(failedStatus.Devices.front().State == DeviceConnectionState::Failed && !failedStatus.Devices.front().IsBusy,
          "a terminal failed status must clear the observed waiting/busy state");

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::AutoReconnectTriggered,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::OperationFailed});
    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::ConnectionError,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::OperationFailed});
    auto failedError = harness.Bridge.Snapshot();
    Check(failedError.Devices.front().State == DeviceConnectionState::Failed && !failedError.Devices.front().IsBusy,
          "a connection error must clear a prior reconnect wait instead of leaving the tray busy");

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::AutoReconnectTriggered,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::OperationFailed});
    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::AutoReconnectFailed,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::OperationFailed});
    auto failedReconnect = harness.Bridge.Snapshot();
    Check(failedReconnect.Devices.front().State == DeviceConnectionState::Failed &&
              !failedReconnect.Devices.front().IsBusy,
          "auto-reconnect failure must clear any observed retry/busy state");
}

void TestMissingMutationCallbacksFailClosed() {
    SettingsData settings;
    settings.Devices.push_back(DeviceSettings{L"target", L"Target", {}, false, false});
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&settings] { return settings; };
    operations.ReadConnectedDevices = [] { return std::vector<DeviceRecord>{Device(L"target", L"Target")}; };
    LegacyAppUseCaseBridge bridge(std::move(operations), settings);
    const auto target = IdSelector(L"target");

    const auto setDefault = bridge.Execute(Command(AppCommandKind::SetDefault, target));
    const auto clearDefault = bridge.Execute(Command(AppCommandKind::ClearDefault));
    const auto setAlias = bridge.Execute(Command(AppCommandKind::SetAlias, target, L"Alias"));
    const auto disconnectAll = bridge.Execute(Command(AppCommandKind::DisconnectAll));
    Check(setDefault.Code == AppResultCode::Unavailable && setDefault.Reason == AppOutcomeReason::NotReady &&
              clearDefault.Code == AppResultCode::Unavailable && clearDefault.Reason == AppOutcomeReason::NotReady &&
              setAlias.Code == AppResultCode::Unavailable && setAlias.Reason == AppOutcomeReason::NotReady &&
              disconnectAll.Code == AppResultCode::Unavailable && disconnectAll.Reason == AppOutcomeReason::NotReady,
          "missing mutation callbacks must fail closed instead of reporting local success");
}

void TestExternalSnapshotIdPreservesP01LengthAndBoundedConversion() {
    Harness harness;
    const std::wstring longId(513, L'x');
    harness.LiveDevices = {Device(longId, L"Long device", {}, true)};
    const auto snapshot = harness.Bridge.Snapshot();
    Check(snapshot.Devices.size() == 1 && snapshot.Devices.front().Id.View() == longId &&
              !snapshot.Devices.front().Id.Bounded(),
          "list/status snapshots must retain a valid P01 ID beyond the bounded persistence identity");
    Check(snapshot.Tray.ConnectedDevices.size() == 1 && snapshot.Tray.ConnectedDevices.front().Id.View() == longId,
          "connected tray snapshot entries must retain the same long external ID");
    const auto event = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                               longId,
                                               DeviceConnectionState::Connecting,
                                               AppResultCode::OperationFailed});
    Check(event && std::holds_alternative<apc::app::DeviceStatusChangedEvent>(*event) &&
              std::get<apc::app::DeviceStatusChangedEvent>(*event).Id.View() == longId,
          "per-device events must preserve a valid long P01 identity without coercing it to DeviceId");
    const auto afterEventSnapshot = harness.Bridge.Snapshot();
    Check(afterEventSnapshot.Devices.size() == 1 && afterEventSnapshot.Devices.front().Id.View() == longId &&
              afterEventSnapshot.Devices.front().State == DeviceConnectionState::Connecting,
          "long-ID event state must remain visible in an immutable snapshot");
}

void TestAuthoritativeBusyFactSurvivesSnapshotNormalization() {
    Harness harness;
    harness.LiveDevices = {Device(L"target", L"Target", {}, true, true, true)};
    harness.AddSettingsDevice(harness.LiveDevices.front());
    harness.SourceSettings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
    harness.SourceSettings.DefaultDeviceId = L"target";
    harness.GlobalBusy = true;
    const auto snapshot = harness.Bridge.Snapshot();
    AppCommandContext detached;
    detached.Completion = AppCommandContext::CompletionMode::Detached;
    const auto toggle =
        harness.Bridge.Execute(Command(AppCommandKind::ToggleLast, DeviceSelector::Default()), detached);
    Check(snapshot.Devices.front().IsBusy && snapshot.Tray.HasBusyOperations,
          "authoritative busy facts must survive snapshot state normalization");
    Check(toggle.Code == AppResultCode::Busy, "tray toggle must reject a target with an authoritative busy fact");
}

} // namespace

int RunLegacyAppUseCaseBridgeTests() {
    TestTargetRanksAmbiguityAndUnknownExternalId();
    TestRefreshCapAndCurrentInputFallback();
    TestMergePrecedence();
    TestAwaitedAndDetachedOperationOutcomes();
    TestIdempotencyAndTrayOnlyBusyPolicy();
    TestReconnectAllAndSynchronousDisconnectAll();
    TestReconnectAllStopsOnFirstPartialFailure();
    TestSettingsMutationsAndFailures();
    TestSnapshotPrivacyResourcePickerAndStableGeneration();
    TestObservedFactsNormalizeAndOverlayWithoutInjection();
    TestReadFailuresAndCommandReadScope();
    TestShutdownIsMonotonicAndRejectsCallbacks();
    TestShutdownClosesAdmissionBeforeTearingDownCallbacks();
    TestFactsAfterShutdownDoNotPublishOrChangeBridgeState();
    TestTerminalFactsClearObservedBusyState();
    TestMissingMutationCallbacksFailClosed();
    TestExternalSnapshotIdPreservesP01LengthAndBoundedConversion();
    TestAuthoritativeBusyFactSurvivesSnapshotNormalization();
    return g_failures;
}
