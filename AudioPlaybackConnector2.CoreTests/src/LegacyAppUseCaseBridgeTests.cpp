#include "TestCheck.hpp"

#include <app/AppController.hpp>
#include <app/LegacyAppUseCaseBridge.hpp>
#include <app/DeviceFactPublicationFence.hpp>
#include <ui/TrayPrimaryActivation.hpp>

#include <algorithm>
#include <array>
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
using apc::app::DeviceFactPublicationFence;
using apc::app::DevicePickerOpenMode;
using apc::app::DeviceSelector;
using apc::app::DeviceSelectorKind;
using apc::app::LegacyAppUseCaseBridge;

using DeviceRecord = LegacyAppUseCaseBridge::DeviceRecord;
using OperationStatus = LegacyAppUseCaseBridge::OperationStatus;

DeviceRecord Device(std::wstring id,
                    std::wstring name,
                    std::wstring deviceAlias = {},
                    bool isConnected = false,
                    bool known = true,
                    bool isBusy = false) {
    return DeviceRecord{std::move(id),
                        std::move(name),
                        std::move(deviceAlias),
                        isConnected ? DeviceConnectionState::Connected : DeviceConnectionState::Idle,
                        isConnected,
                        known,
                        isBusy};
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
    std::function<SettingsSnapshot()> ReadSettings;
    std::uint64_t SettingsRevision = 0;
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
    bool DisconnectRemovesSession = false;
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
    std::vector<DevicePickerOpenMode> PickerOpenModes;
    AppSnapshot::ResourceStatusSnapshot Resource;
    std::uint64_t PickerGeneration = 0;
    std::chrono::steady_clock::time_point LastRefreshDeadline{};
    LegacyAppUseCaseBridge Bridge;

    Harness()
        : SourceSettings(), LiveDevices(), RefreshDevices(), ReadSettings([this] {
              ++SettingsReadCalls;
              if (SettingsReadFails) throw 1;
              return SettingsSnapshot{SourceSettings, SettingsRevision, false};
          }),
          Bridge(MakeOperations()) {}

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
                if (found == LiveDevices.end()) {
                    LiveDevices.push_back(Device(std::wstring(id), std::wstring(id), {}, true, true));
                } else {
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
                if (DisconnectRemovesSession) {
                    LiveDevices.erase(found);
                    return;
                }
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
            ++SettingsRevision;
            return true;
        };
        operations.ClearDefaultDevice = [this] {
            ++ClearDefaultCalls;
            if (!ClearDefaultAccepted) return false;
            SourceSettings.DefaultDevice = ::DefaultDeviceMode::LastConnected;
            SourceSettings.DefaultDeviceId.clear();
            ++SettingsRevision;
            return true;
        };
        operations.SetDeviceAlias = [this](std::wstring_view id, std::wstring_view alias, std::wstring_view) {
            ++SetAliasCalls;
            if (!SetAliasAccepted) return false;
            auto found =
                std::ranges::find_if(SourceSettings.Devices, [id](auto const& device) { return device.Id == id; });
            if (found == SourceSettings.Devices.end()) return false;
            found->Alias = alias;
            ++SettingsRevision;
            return true;
        };
        operations.ShowDevicePicker = [this](DevicePickerOpenMode openMode, AppCommandContext const&) {
            PickerOpenModes.push_back(openMode);
            return PickerResponse;
        };
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

    harness.RefreshResponse = {OperationStatus::TimedOut, {}};
    const auto liveTarget = DeviceSelector::ByQuery(DeviceSelectorKind::Name, L"Live");
    const auto setDefault = harness.Bridge.Execute(Command(AppCommandKind::SetDefault, liveTarget));
    Check(setDefault.Code == AppResultCode::Success && harness.SetDefaultCalls == 1,
          "a private refresh-cap timeout must retain the live-input fallback while the original context remains live");
}

void TestMutationAdmissionRejectsCancellationAfterRefreshFallback() {
    SettingsData settings;
    std::uint64_t settingsRevision = 0;
    const auto liveDevice = Device(L"device-id", L"Headphones", L"Desk");
    settings.Devices.push_back(DeviceSettings{liveDevice.Id, liveDevice.Name, liveDevice.Alias, false, false});
    std::mutex refreshMutex;
    std::condition_variable refreshChanged;
    bool refreshEntered = false;
    bool releaseRefresh = false;
    std::atomic<int> defaultMutations = 0;
    std::atomic<int> aliasMutations = 0;

    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] { return SettingsSnapshot{settings, settingsRevision, false}; };
    operations.ReadConnectedDevices = [&] { return std::vector<DeviceRecord>{liveDevice}; };
    operations.Refresh = [&](AppCommandContext const&) {
        {
            std::scoped_lock lock(refreshMutex);
            refreshEntered = true;
        }
        refreshChanged.notify_all();
        std::unique_lock lock(refreshMutex);
        refreshChanged.wait(lock, [&] { return releaseRefresh; });
        return LegacyAppUseCaseBridge::RefreshResult{OperationStatus::Cancelled, {}};
    };
    operations.SetDefaultDevice = [&](std::wstring_view) {
        ++defaultMutations;
        ++settingsRevision;
        return true;
    };
    operations.SetDeviceAlias = [&](std::wstring_view, std::wstring_view, std::wstring_view) {
        ++aliasMutations;
        ++settingsRevision;
        return true;
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));

    const auto runCancelledMutation = [&](AppCommand command,
                                          AppResultCode expectedCode,
                                          std::string_view description) {
        std::stop_source stopSource;
        AppCommandContext context;
        context.StopToken = stopSource.get_token();
        AppResultCode resultCode = AppResultCode::InternalError;
        std::thread workerThread([&] { resultCode = bridge.Execute(std::move(command), context).Code; });
        {
            std::unique_lock lock(refreshMutex);
            const bool entered = refreshChanged.wait_for(lock, std::chrono::seconds(1), [&] { return refreshEntered; });
            Check(entered, "the mutation refresh must enter before cancellation is requested");
        }
        stopSource.request_stop();
        {
            std::scoped_lock lock(refreshMutex);
            releaseRefresh = true;
        }
        refreshChanged.notify_all();
        workerThread.join();
        Check(resultCode == expectedCode, description);
    };

    const auto namedTarget = DeviceSelector::ByQuery(DeviceSelectorKind::Name, L"Headphones");
    runCancelledMutation(Command(AppCommandKind::SetDefault, namedTarget),
                         AppResultCode::Cancelled,
                         "a cancelled name-refresh set-default must report cancellation after controller dispatch");
    Check(defaultMutations.load() == 0,
          "a cancelled name-refresh set-default must not invoke its settings mutation callback");

    {
        std::scoped_lock lock(refreshMutex);
        refreshEntered = false;
        releaseRefresh = false;
    }
    const auto automaticTarget = DeviceSelector::ByQuery(DeviceSelectorKind::Auto, L"Headphones");
    runCancelledMutation(Command(AppCommandKind::SetAlias, automaticTarget, L"Office"),
                         AppResultCode::Cancelled,
                         "a cancelled auto-refresh set-alias must report cancellation after controller dispatch");
    Check(aliasMutations.load() == 0,
          "a cancelled auto-refresh set-alias must not invoke its settings mutation callback");
}

void TestMutationAdmissionRejectsDeadlineAfterRefreshFallback() {
    SettingsData settings;
    std::uint64_t settingsRevision = 0;
    const auto liveDevice = Device(L"device-id", L"Headphones", L"Desk");
    settings.Devices.push_back(DeviceSettings{liveDevice.Id, liveDevice.Name, liveDevice.Alias, false, false});
    std::mutex refreshMutex;
    std::condition_variable refreshChanged;
    bool refreshEntered = false;
    std::atomic<int> aliasMutations = 0;

    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] { return SettingsSnapshot{settings, settingsRevision, false}; };
    operations.ReadConnectedDevices = [&] { return std::vector<DeviceRecord>{liveDevice}; };
    operations.Refresh = [&](AppCommandContext const& context) {
        {
            std::scoped_lock lock(refreshMutex);
            refreshEntered = true;
        }
        refreshChanged.notify_all();
        std::unique_lock lock(refreshMutex);
        refreshChanged.wait_until(
            lock, context.Deadline, [&] { return context.IsExpired(AppCommandContext::Clock::now()); });
        return LegacyAppUseCaseBridge::RefreshResult{OperationStatus::TimedOut, {}};
    };
    operations.SetDeviceAlias = [&](std::wstring_view, std::wstring_view, std::wstring_view) {
        ++aliasMutations;
        ++settingsRevision;
        return true;
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));
    const auto automaticTarget = DeviceSelector::ByQuery(DeviceSelectorKind::Auto, L"Headphones");
    AppCommandContext context;
    context.Deadline = AppCommandContext::Clock::now() + std::chrono::milliseconds(100);
    const auto result = bridge.Execute(Command(AppCommandKind::SetAlias, automaticTarget, L"Office"), context);
    {
        std::scoped_lock lock(refreshMutex);
        Check(refreshEntered, "the deadline test must begin its refresh before the original command expires");
    }
    Check(result.Code == AppResultCode::TimedOut && aliasMutations.load() == 0,
          "an expired auto-refresh command must not invoke its alias mutation callback after fallback resolution");
}

void TestMutationAdmissionRejectsLateCancellationWithoutRefresh() {
    SettingsData settings;
    std::uint64_t settingsRevision = 0;
    settings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
    settings.DefaultDeviceId = L"device-id";
    std::stop_source stop;
    std::atomic<int> clearDefaultMutations = 0;
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] {
        stop.request_stop();
        return SettingsSnapshot{settings, settingsRevision, false};
    };
    operations.ClearDefaultDevice = [&] {
        ++clearDefaultMutations;
        ++settingsRevision;
        return true;
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));
    AppCommandContext context;
    context.StopToken = stop.get_token();
    const auto result = bridge.Execute(Command(AppCommandKind::ClearDefault), context);
    Check(result.Code == AppResultCode::Cancelled && clearDefaultMutations.load() == 0,
          "cancellation after command admission but before a non-refresh callback must prevent the mutation");
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
              harness.SetDefaultCalls == 1 && setDefault.DefaultDevice && setDefault.DefaultDevice->Id &&
              setDefault.DefaultDevice->Id->View() == L"target",
          "set-default must call the injected setter and immediately return committed settings");
    auto defaultSnapshot = harness.Bridge.Snapshot();
    Check(defaultSnapshot.DefaultDevice &&
              defaultSnapshot.DefaultDevice->Mode == apc::app::DefaultDeviceMode::SpecificDevice &&
              defaultSnapshot.DefaultDevice->Id && defaultSnapshot.DefaultDevice->Id->View() == L"target",
          "set-default must expose the committed Store settings snapshot");

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
              harness.ClearDefaultCalls == 1 && clearDefault.DefaultDevice &&
              clearDefault.DefaultDevice->Mode == apc::app::DefaultDeviceMode::LastConnected,
          "clear-default must call the injected setter and immediately return committed settings");

    const std::wstring longId(513, L'z');
    harness.LiveDevices.push_back(Device(longId, L"Long External ID"));
    harness.AddSettingsDevice(harness.LiveDevices.back());
    const auto longDefault = harness.Bridge.Execute(Command(AppCommandKind::SetDefault, DeviceSelector::ById(longId)));
    Check(longDefault.Code == AppResultCode::Success && harness.SetDefaultCalls == 3 &&
              harness.SourceSettings.DefaultDeviceId == longId,
          "set-default must pass a resolved live external ID beyond the persistence DeviceId bound to the setter");
}

void TestSettingsRevisionFenceAdvancesGenerationOnce() {
    Harness harness;
    const auto initial = harness.Bridge.Snapshot();

    harness.SourceSettings.PrivacyModeEnabled = true;
    ++harness.SettingsRevision;
    const auto changed = harness.Bridge.Snapshot();
    const auto repeated = harness.Bridge.Snapshot();

    Check(!initial.PrivacyModeEnabled && changed.PrivacyModeEnabled && changed.Generation == initial.Generation + 1 &&
              repeated.Generation == changed.Generation,
          "an external Store revision must refresh snapshot content and advance generation exactly once");
}

void TestSettingsRevisionFenceIgnoresStaleReads() {
    SettingsData settings;
    const std::array<std::uint64_t, 5> revisions{2, 1, 2, 2, 3};
    std::size_t nextRevision = 0;

    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] {
        const auto revision = revisions[nextRevision++];
        return SettingsSnapshot{settings, revision, false};
    };
    operations.ReadConnectedDevices = [] { return std::vector<DeviceRecord>{}; };

    LegacyAppUseCaseBridge bridge(std::move(operations));
    const auto baseline = bridge.Snapshot();
    const auto stale = bridge.Snapshot();
    const auto repeated = bridge.Snapshot();
    const auto newer = bridge.Snapshot();

    Check(baseline.Generation == 0 && stale.Generation == 0 && repeated.Generation == 0 && newer.Generation == 1,
          "stale or equal Store revisions must not regress or repeatedly advance bridge generation");
}

void TestConcurrentSettingsReadsKeepSnapshotsAndStatusResultsCoherent() {
    const auto makeSettings = [](std::wstring defaultId, std::wstring alias, bool privacy) {
        SettingsData settings;
        settings.PrivacyModeEnabled = privacy;
        settings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
        settings.DefaultDeviceId = std::move(defaultId);
        settings.Devices = {DeviceSettings{L"target", L"Target", std::move(alias), false, false},
                            DeviceSettings{settings.DefaultDeviceId, L"Default", {}, false, false}};
        return settings;
    };
    const auto oldSettings = makeSettings(L"old-default", L"Old alias", false);
    const auto newSettings = makeSettings(L"new-default", L"New alias", true);

    const auto isNewSnapshot = [](AppSnapshot const& snapshot) {
        const auto target =
            std::ranges::find_if(snapshot.Devices, [](auto const& device) { return device.Id.View() == L"target"; });
        return snapshot.IsRunning && snapshot.PrivacyModeEnabled && snapshot.DefaultDevice &&
               snapshot.DefaultDevice->Id && snapshot.DefaultDevice->Id->View() == L"new-default" &&
               target != snapshot.Devices.end() && target->Alias == L"New alias";
    };

    const auto runInterleaving = [&](bool executeStatus) {
        std::mutex callbackMutex;
        std::condition_variable callbackEntered;
        std::condition_variable callbackRelease;
        int readCalls = 0;
        bool firstReadBlocked = false;
        bool secondReadBlocked = false;
        bool releaseFirst = false;
        bool releaseSecond = false;

        LegacyAppUseCaseBridge::Operations operations;
        operations.ReadSettings = [&] {
            std::unique_lock lock(callbackMutex);
            ++readCalls;
            if (readCalls == 1) return SettingsSnapshot{oldSettings, 1, false};
            if (readCalls == 2) {
                firstReadBlocked = true;
                callbackEntered.notify_all();
                callbackRelease.wait(lock, [&] { return releaseFirst; });
                return SettingsSnapshot{oldSettings, 1, false};
            }
            if (readCalls == 3) {
                secondReadBlocked = true;
                callbackEntered.notify_all();
                callbackRelease.wait(lock, [&] { return releaseSecond; });
                return SettingsSnapshot{newSettings, 2, false};
            }
            return SettingsSnapshot{newSettings, 2, false};
        };
        operations.ReadConnectedDevices = [] {
            return std::vector<DeviceRecord>{
                Device(L"target", L"Target"), Device(L"old-default", L"Default"), Device(L"new-default", L"Default")};
        };
        LegacyAppUseCaseBridge bridge(std::move(operations));
        const auto baseline = bridge.Snapshot();

        AppSnapshot staleSnapshot;
        AppSnapshot currentSnapshot;
        apc::app::AppResult status;
        std::thread staleReader([&] {
            if (executeStatus) {
                status = bridge.Execute(Command(AppCommandKind::Status));
            } else {
                staleSnapshot = bridge.Snapshot();
            }
        });
        {
            std::unique_lock lock(callbackMutex);
            callbackEntered.wait(lock, [&] { return firstReadBlocked; });
        }
        std::thread currentReader([&] { currentSnapshot = bridge.Snapshot(); });
        {
            std::unique_lock lock(callbackMutex);
            callbackEntered.wait(lock, [&] { return secondReadBlocked; });
            releaseSecond = true;
        }
        callbackRelease.notify_all();
        currentReader.join();
        {
            std::scoped_lock lock(callbackMutex);
            releaseFirst = true;
        }
        callbackRelease.notify_all();
        staleReader.join();

        const auto staleOutput = executeStatus ? status.Snapshot : std::optional<AppSnapshot>{staleSnapshot};
        const bool statusDevicesAreNew = !executeStatus || std::ranges::any_of(status.Devices, [](auto const& device) {
            return device.Id.View() == L"target" && device.Alias == L"New alias";
        });
        Check(
            isNewSnapshot(currentSnapshot) && staleOutput && isNewSnapshot(*staleOutput) &&
                currentSnapshot.Generation == baseline.Generation + 1 &&
                staleOutput->Generation == currentSnapshot.Generation && statusDevicesAreNew,
            executeStatus
                ? "a stale status read must retry so result devices and its published snapshot retain one new revision"
                : "a stale snapshot read must retry instead of combining old privacy/default/alias data with a newer "
                  "generation");
    };

    runInterleaving(false);
    runInterleaving(true);
}

void TestMutationResultsUseCommittedSettings() {
    SettingsData settings;
    std::uint64_t settingsRevision = 0;
    settings.Devices.push_back(DeviceSettings{L"target", L"Target", L"Before", false, false});

    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&settings, &settingsRevision] {
        return SettingsSnapshot{settings, settingsRevision, false};
    };
    operations.ReadConnectedDevices = [] { return std::vector<DeviceRecord>{Device(L"target", L"Target")}; };
    operations.SetDefaultDevice = [&settings, &settingsRevision](std::wstring_view) {
        settings.DefaultDevice = ::DefaultDeviceMode::SpecificDevice;
        settings.DefaultDeviceId = L"committed-default";
        ++settingsRevision;
        return true;
    };
    operations.SetDeviceAlias = [&settings,
                                 &settingsRevision](std::wstring_view id, std::wstring_view, std::wstring_view) {
        auto device = std::ranges::find(settings.Devices, id, &DeviceSettings::Id);
        if (device == settings.Devices.end()) return false;
        device->Alias = L"Committed Alias";
        ++settingsRevision;
        return true;
    };

    LegacyAppUseCaseBridge bridge(std::move(operations));
    const auto target = IdSelector(L"target");
    const auto defaultResult = bridge.Execute(Command(AppCommandKind::SetDefault, target));
    Check(defaultResult.DefaultDevice && defaultResult.DefaultDevice->Id &&
              defaultResult.DefaultDevice->Id->View() == L"committed-default",
          "set-default results must be built from the reread committed settings value");

    const auto aliasResult = bridge.Execute(Command(AppCommandKind::SetAlias, target, L"Requested Alias"));
    Check(aliasResult.Alias == L"Committed Alias" && aliasResult.Device &&
              aliasResult.Device->Alias == L"Committed Alias",
          "alias results must expose the reread committed alias instead of the pre-commit request");
}

void TestUnpersistableExternalDefaultRetainsP01Success() {
    SettingsData settings;
    std::uint64_t settingsRevision = 1;
    const std::wstring longId(513, L'x');
    int defaultMutations = 0;

    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] { return SettingsSnapshot{settings, settingsRevision, false}; };
    operations.ReadConnectedDevices = [&] {
        return std::vector<DeviceRecord>{Device(longId, L"Long external ID"), Device(L"bounded-id", L"Bounded ID")};
    };
    // The long ID models the SettingsStore boundary rejection caused by the
    // P07 persistence identity bound. The bounded ID models an independent
    // persistence failure so the bridge keeps reporting that failure.
    operations.SetDefaultDevice = [&](std::wstring_view id) {
        ++defaultMutations;
        return id != L"bounded-id" && apc::core::DeviceId::TryCreate(id).has_value();
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));

    const auto longResult = bridge.Execute(Command(AppCommandKind::SetDefault, IdSelector(longId)));
    Check(longResult.Code == AppResultCode::Success && longResult.Reason == AppOutcomeReason::DefaultSet &&
              longResult.Target && longResult.Target->Id == longId && defaultMutations == 1,
          "a Store-rejected unpersistable but live P01 ID must retain the set-default success outcome");

    const auto boundedResult = bridge.Execute(Command(AppCommandKind::SetDefault, IdSelector(L"bounded-id")));
    Check(boundedResult.Code == AppResultCode::OperationFailed &&
              boundedResult.Reason == AppOutcomeReason::InternalError && defaultMutations == 2,
          "a rejected bounded persistence ID must remain an observable set-default operation failure");
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

void TestPickerOpenModePreservesTrayToggleAndControlEnsureOpen() {
    Harness harness;
    harness.PickerResponse = {OperationStatus::Succeeded, 11};

    const auto controlShow = harness.Bridge.Execute(Command(AppCommandKind::ShowDevicePicker));
    Check(controlShow.Code == AppResultCode::Success && !harness.PickerOpenModes.empty() &&
              harness.PickerOpenModes.back() == DevicePickerOpenMode::EnsureOpen,
          "control show must retain idempotent ensure-open picker semantics");

    apc::app::AppResult trayShow;
    auto controller = std::make_shared<apc::app::AppController>(
        [&](AppCommand const& command, AppCommandContext const& context) {
            trayShow = harness.Bridge.Execute(command, context);
            return trayShow;
        },
        [&] { return harness.Bridge.Snapshot(); });
    auto callback = apc::ui::MakeTrayPrimaryActivationCallback(controller);
    callback();
    Check(trayShow.Code == AppResultCode::Success && harness.PickerOpenModes.size() == 2 &&
              harness.PickerOpenModes.back() == DevicePickerOpenMode::ToggleIfOpen,
          "tray primary activation must carry toggle-if-open semantics through the shared bridge");
}

void TestObservedFactsNormalizeWithoutOwningSessionState() {
    Harness harness;
    harness.AddSettingsDevice(Device(L"target", L"Target"));
    const auto before = harness.Bridge.Snapshot().Generation;
    auto status = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                          L"target",
                                          DeviceConnectionState::Connecting,
                                          AppResultCode::OperationFailed});
    Check(status && std::holds_alternative<apc::app::DeviceStatusChangedEvent>(*status),
          "legacy status facts must normalize to typed status events");
    auto afterStatus = harness.Bridge.Snapshot();
    Check(afterStatus.Generation > before && afterStatus.Devices.size() == 1 &&
              afterStatus.Devices.front().State == DeviceConnectionState::Idle && !afterStatus.Devices.front().IsBusy,
          "observed status must advance publication generation without inventing a session state");

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

void TestSettingsReadCallbackIsRequired() {
    LegacyAppUseCaseBridge::Operations operations;
    LegacyAppUseCaseBridge bridge(std::move(operations));

    const auto result = bridge.Execute(Command(AppCommandKind::Status));
    const auto snapshot = bridge.Snapshot();
    Check(result.Code == AppResultCode::InternalError && !snapshot.IsRunning,
          "commands and snapshots must fail closed when the required settings callback is absent");
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
    std::uint64_t settingsRevision = 0;
    int settingsMutationCalls = 0;
    int settingsUiCalls = 0;
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] {
        ++settingsReadCalls;
        return SettingsSnapshot{settings, settingsRevision, false};
    };
    operations.ClearDefaultDevice = [&] {
        std::unique_lock lock(callbackMutex);
        hasEnteredCallback = true;
        callbackEntered.notify_all();
        releaseCallback.wait(lock, [&] { return canCompleteCallback; });
        ++settingsMutationCalls;
        ++settingsRevision;
        return true;
    };
    operations.ShowSettings = [&](AppCommandContext const&) {
        ++settingsUiCalls;
        return LegacyAppUseCaseBridge::UiActionResult{OperationStatus::Succeeded, std::nullopt};
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));
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

void TestSessionStateDoesNotDependOnFactDelivery() {
    Harness harness;
    harness.GlobalBusy = true;
    harness.AddSettingsDevice(Device(L"target", L"Target"));
    harness.LiveDevices = {Device(L"target", L"Target", {}, false, true, true)};
    harness.LiveDevices.front().State = DeviceConnectionState::WaitingForReconnect;

    auto waiting = harness.Bridge.Snapshot();
    Check(waiting.Devices.front().State == DeviceConnectionState::WaitingForReconnect && waiting.Devices.front().IsBusy,
          "a session's reconnect wait must be visible before presentation facts are delivered");

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::ConnectionError,
                                  L"target",
                                  DeviceConnectionState::Failed,
                                  AppResultCode::OperationFailed});
    auto afterOldFailure = harness.Bridge.Snapshot();
    Check(afterOldFailure.Devices.front().State == DeviceConnectionState::WaitingForReconnect &&
              afterOldFailure.Devices.front().IsBusy,
          "a delayed failure fact must not overwrite a newer waiting session");

    harness.LiveDevices.front().State = DeviceConnectionState::Failed;
    harness.LiveDevices.front().IsBusy = false;
    auto failed = harness.Bridge.Snapshot();
    Check(failed.Devices.front().State == DeviceConnectionState::Failed && !failed.Devices.front().IsBusy &&
              !failed.Tray.HasBusyOperations,
          "terminal session state must clear busy before the terminal fact reaches presentation");

    (void)harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::AutoReconnectTriggered,
                                  L"target",
                                  DeviceConnectionState::Idle,
                                  AppResultCode::Success});
    auto afterOldRetry = harness.Bridge.Snapshot();
    Check(afterOldRetry.Devices.front().State == DeviceConnectionState::Failed && !afterOldRetry.Devices.front().IsBusy,
          "a delayed retry fact must not revive a terminal session's busy state");

    harness.LiveDevices.clear();
    auto removed = harness.Bridge.Snapshot();
    Check(removed.Devices.front().State == DeviceConnectionState::Idle && !removed.Devices.front().IsBusy &&
              !removed.Devices.front().IsConnected,
          "removing a session must discard its runtime state even without a disconnected fact");
}

void TestRefreshUsesSessionStateAfterEnumerationCompletes() {
    bool connected = true;
    SettingsData settings;
    settings.Devices.push_back({L"target", L"Target", {}, false, false});
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] { return SettingsSnapshot{settings, 0, false}; };
    operations.ReadConnectedDevices = [&] {
        return std::vector<DeviceRecord>{Device(L"target", L"Target", {}, connected)};
    };
    operations.Refresh = [&](AppCommandContext const&) {
        connected = false;
        return LegacyAppUseCaseBridge::RefreshResult{OperationStatus::Succeeded, {Device(L"target", L"Discovered")}};
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));
    const auto result = bridge.Execute(Command(AppCommandKind::ListDevices));
    Check(result.Succeeded() && result.Devices.size() == 1 && !result.Devices.front().IsConnected &&
              result.Devices.front().State == DeviceConnectionState::Idle,
          "a refresh must use session truth read after enumeration, not an earlier connected snapshot");
}

void TestMissingMutationCallbacksFailClosed() {
    SettingsData settings;
    std::uint64_t settingsRevision = 0;
    settings.Devices.push_back(DeviceSettings{L"target", L"Target", {}, false, false});
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&settings, &settingsRevision] {
        return SettingsSnapshot{settings, settingsRevision, false};
    };
    operations.ReadConnectedDevices = [] { return std::vector<DeviceRecord>{Device(L"target", L"Target")}; };
    LegacyAppUseCaseBridge bridge(std::move(operations));
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

    Harness disconnected;
    disconnected.AddSettingsDevice(Device(longId, L"Long device"));
    disconnected.LiveDevices = {Device(longId, L"Long device", {}, false, true, true)};
    disconnected.LiveDevices.front().State = DeviceConnectionState::Connecting;
    const auto event = disconnected.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                                    longId,
                                                    DeviceConnectionState::Connecting,
                                                    AppResultCode::OperationFailed});
    Check(event && std::holds_alternative<apc::app::DeviceStatusChangedEvent>(*event) &&
              std::get<apc::app::DeviceStatusChangedEvent>(*event).Id.View() == longId,
          "per-device events must preserve a valid long P01 identity without coercing it to DeviceId");
    const auto afterEventSnapshot = disconnected.Bridge.Snapshot();
    Check(afterEventSnapshot.Devices.size() == 1 && afterEventSnapshot.Devices.front().Id.View() == longId &&
              afterEventSnapshot.Devices.front().State == DeviceConnectionState::Connecting,
          "long-ID owner state must remain visible in an immutable snapshot");
}

void TestSessionTruthIgnoresDelayedConnectionFacts() {
    Harness disconnect;
    disconnect.LiveDevices = {Device(L"target", L"Target", {}, true)};
    disconnect.AddSettingsDevice(disconnect.LiveDevices.front());
    disconnect.DisconnectRemovesSession = true;
    (void)disconnect.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceConnected,
                                     L"target",
                                     DeviceConnectionState::Connected,
                                     AppResultCode::Success});
    const auto disconnected = disconnect.Bridge.Execute(Command(AppCommandKind::Disconnect, IdSelector(L"target")));
    const auto disconnectSnapshot = disconnect.Bridge.Snapshot();
    Check(disconnected.Code == AppResultCode::Success && disconnected.Reason == AppOutcomeReason::DisconnectSucceeded &&
              disconnected.Device && !disconnected.Device->IsConnected && disconnectSnapshot.Devices.size() == 1 &&
              !disconnectSnapshot.Devices.front().IsConnected &&
              disconnectSnapshot.Devices.front().State == DeviceConnectionState::Idle,
          "a stale connected fact must not revive a session removed by a successful disconnect");

    Harness connect;
    connect.AddSettingsDevice(Device(L"target", L"Target"));
    (void)connect.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                  L"target",
                                  DeviceConnectionState::Failed,
                                  AppResultCode::OperationFailed});
    const auto connected = connect.Bridge.Execute(Command(AppCommandKind::Connect, IdSelector(L"target")));
    Check(connected.Code == AppResultCode::Success && connected.Reason == AppOutcomeReason::ConnectSucceeded &&
              connected.Device && connected.Device->IsConnected && connect.ConnectCalls == 1,
          "a failed observed state must not make a source-confirmed connect appear to fail");

    Harness reconnect;
    reconnect.AddSettingsDevice(Device(L"target", L"Target"));
    (void)reconnect.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                    L"target",
                                    DeviceConnectionState::Idle,
                                    AppResultCode::Success});
    const auto reconnected = reconnect.Bridge.Execute(Command(AppCommandKind::Reconnect, IdSelector(L"target")));
    Check(reconnected.Code == AppResultCode::Success && reconnected.Reason == AppOutcomeReason::ReconnectSucceeded &&
              reconnected.Device && reconnected.Device->IsConnected && reconnect.ReconnectCalls == 1,
          "an idle observed state must not make a source-confirmed reconnect appear to fail");

    Harness sourceConnected;
    sourceConnected.LiveDevices = {Device(L"target", L"Target", {}, true)};
    sourceConnected.AddSettingsDevice(sourceConnected.LiveDevices.front());
    (void)sourceConnected.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                          L"target",
                                          DeviceConnectionState::Failed,
                                          AppResultCode::OperationFailed});
    const auto sourceConnectedSnapshot = sourceConnected.Bridge.Snapshot();
    Check(sourceConnectedSnapshot.Devices.size() == 1 && sourceConnectedSnapshot.Devices.front().IsConnected &&
              sourceConnectedSnapshot.Devices.front().State == DeviceConnectionState::Connected,
          "the source session record must remain available after merge to reject a stale failed overlay");

    Harness staleConnected;
    staleConnected.AddSettingsDevice(Device(L"target", L"Target"));
    (void)staleConnected.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceConnected,
                                         L"target",
                                         DeviceConnectionState::Connected,
                                         AppResultCode::Success});
    const auto connectAfterStaleFact =
        staleConnected.Bridge.Execute(Command(AppCommandKind::Connect, IdSelector(L"target")));
    Check(connectAfterStaleFact.Code == AppResultCode::Success &&
              connectAfterStaleFact.Reason == AppOutcomeReason::ConnectSucceeded && staleConnected.ConnectCalls == 1,
          "a stale connected fact must not turn a source-disconnected connect into already-connected");

    Harness transient;
    transient.AddSettingsDevice(Device(L"target", L"Target"));
    transient.LiveDevices = {Device(L"target", L"Target", {}, false, true, true)};
    transient.LiveDevices.front().State = DeviceConnectionState::Connecting;
    (void)transient.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                    L"target",
                                    DeviceConnectionState::Connecting,
                                    AppResultCode::Success});
    const auto connectingSnapshot = transient.Bridge.Snapshot();
    transient.LiveDevices.front().State = DeviceConnectionState::Failed;
    transient.LiveDevices.front().IsBusy = false;
    (void)transient.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                    L"target",
                                    DeviceConnectionState::Failed,
                                    AppResultCode::OperationFailed});
    const auto failedSnapshot = transient.Bridge.Snapshot();
    Check(connectingSnapshot.Devices.front().State == DeviceConnectionState::Connecting &&
              !connectingSnapshot.Devices.front().IsConnected && connectingSnapshot.Devices.front().IsBusy &&
              failedSnapshot.Devices.front().State == DeviceConnectionState::Failed &&
              !failedSnapshot.Devices.front().IsConnected && !failedSnapshot.Devices.front().IsBusy,
          "non-connected session states must remain visible through the presentation merge");
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

void TestQueuedConnectedFactCannotResurrectClosedSnapshot() {
    Harness harness;
    harness.AddSettingsDevice(Device(L"target", L"Target"));
    DeviceFactPublicationFence fence;

    const auto queuedConnected = fence.RecordConnected(L"target");
    const auto queuedDisconnected = fence.RecordDisconnected(L"target");
    const auto queuedIdle = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::None);
    const auto connectedFactSuperseded = !fence.IsCurrent(queuedConnected);

    std::optional<AppEvent> connectedEvent;
    if (fence.IsCurrent(queuedConnected)) {
        connectedEvent = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceConnected,
                                                 L"target",
                                                 DeviceConnectionState::Connected,
                                                 AppResultCode::Success});
    }
    std::optional<AppEvent> disconnectedEvent;
    if (fence.IsCurrent(queuedDisconnected)) {
        disconnectedEvent = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceDisconnected,
                                                    L"target",
                                                    DeviceConnectionState::Idle,
                                                    AppResultCode::Success});
    }
    std::optional<AppEvent> idleEvent;
    if (fence.IsCurrent(queuedIdle)) {
        idleEvent = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                            L"target",
                                            DeviceConnectionState::Idle,
                                            AppResultCode::Success});
    }

    const auto snapshot = harness.Bridge.Snapshot();
    Check(connectedFactSuperseded && !connectedEvent && disconnectedEvent && idleEvent &&
              snapshot.Devices.size() == 1 && snapshot.Devices.front().State == DeviceConnectionState::Idle &&
              !snapshot.Devices.front().IsConnected && !snapshot.Tray.HasBusyOperations,
          "a queued connected fact superseded by close must not publish an event or resurrect the closed snapshot");
}

void TestQueuedStatusFactCannotOverwriteNewerStatus() {
    Harness harness;
    harness.AddSettingsDevice(Device(L"target", L"Target"));
    harness.LiveDevices = {Device(L"target", L"Target")};
    harness.LiveDevices.front().State = DeviceConnectionState::Failed;
    DeviceFactPublicationFence fence;

    const auto queuedConnected = fence.RecordConnected(L"target");
    const auto queuedConnecting = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::Connecting);
    const auto queuedFailure = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::Error);
    const auto connectedFactSuperseded = !fence.IsCurrent(queuedConnected);

    std::optional<AppEvent> connectingEvent;
    if (fence.IsCurrent(queuedConnecting)) {
        connectingEvent = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                                  L"target",
                                                  DeviceConnectionState::Connecting,
                                                  AppResultCode::Success});
    }
    std::optional<AppEvent> failureEvent;
    if (fence.IsCurrent(queuedFailure)) {
        failureEvent = harness.Bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                               L"target",
                                               DeviceConnectionState::Failed,
                                               AppResultCode::OperationFailed});
    }

    const auto snapshot = harness.Bridge.Snapshot();
    Check(connectedFactSuperseded && !connectingEvent && failureEvent && snapshot.Devices.size() == 1 &&
              snapshot.Devices.front().State == DeviceConnectionState::Failed && !snapshot.Devices.front().IsBusy,
          "a queued status fact superseded before UI drain must not publish or overwrite the newer typed state");
}

void TestDuplicateQueuedFactsRemainCurrentUntilTheStateChanges() {
    DeviceFactPublicationFence fence;

    const auto firstConnecting = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::Connecting);
    const auto duplicateConnecting = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::Connecting);
    const auto duplicatesCurrent = fence.IsCurrent(firstConnecting) && fence.IsCurrent(duplicateConnecting);
    const auto connected = fence.RecordConnected(L"target");
    const auto connectedStatus = fence.RecordStatus(L"target", DeviceFactPublicationFence::Status::Connected);

    Check(duplicatesCurrent && !fence.IsCurrent(firstConnecting) && !fence.IsCurrent(duplicateConnecting) &&
              fence.IsCurrent(connected) && fence.IsCurrent(connectedStatus),
          "duplicate source facts and paired connected status must remain publishable while their state is current");
}

} // namespace

int RunLegacyAppUseCaseBridgeTests() {
    TestTargetRanksAmbiguityAndUnknownExternalId();
    TestRefreshCapAndCurrentInputFallback();
    TestMutationAdmissionRejectsCancellationAfterRefreshFallback();
    TestMutationAdmissionRejectsDeadlineAfterRefreshFallback();
    TestMutationAdmissionRejectsLateCancellationWithoutRefresh();
    TestMergePrecedence();
    TestAwaitedAndDetachedOperationOutcomes();
    TestIdempotencyAndTrayOnlyBusyPolicy();
    TestReconnectAllAndSynchronousDisconnectAll();
    TestReconnectAllStopsOnFirstPartialFailure();
    TestSettingsMutationsAndFailures();
    TestSettingsRevisionFenceAdvancesGenerationOnce();
    TestSettingsRevisionFenceIgnoresStaleReads();
    TestConcurrentSettingsReadsKeepSnapshotsAndStatusResultsCoherent();
    TestMutationResultsUseCommittedSettings();
    TestUnpersistableExternalDefaultRetainsP01Success();
    TestSnapshotPrivacyResourcePickerAndStableGeneration();
    TestPickerOpenModePreservesTrayToggleAndControlEnsureOpen();
    TestObservedFactsNormalizeWithoutOwningSessionState();
    TestReadFailuresAndCommandReadScope();
    TestSettingsReadCallbackIsRequired();
    TestShutdownIsMonotonicAndRejectsCallbacks();
    TestShutdownClosesAdmissionBeforeTearingDownCallbacks();
    TestFactsAfterShutdownDoNotPublishOrChangeBridgeState();
    TestSessionStateDoesNotDependOnFactDelivery();
    TestRefreshUsesSessionStateAfterEnumerationCompletes();
    TestMissingMutationCallbacksFailClosed();
    TestExternalSnapshotIdPreservesP01LengthAndBoundedConversion();
    TestSessionTruthIgnoresDelayedConnectionFacts();
    TestAuthoritativeBusyFactSurvivesSnapshotNormalization();
    TestQueuedConnectedFactCannotResurrectClosedSnapshot();
    TestQueuedStatusFactCannotOverwriteNewerStatus();
    TestDuplicateQueuedFactsRemainCurrentUntilTheStateChanges();
    return g_failures;
}
