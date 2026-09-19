#include <app/AppController.hpp>
#include <app/StartupTaskCoordinator.hpp>
#include <core/DeviceService.hpp>
#include <winrt/Windows.Foundation.h>

#include <core/SettingsLimits.hpp>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace apc::app {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Local Helpers /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

using OperationStatus = AppActionStatus;

AppResult InvalidInput(AppCommandKind kind) {
    return {AppResultCode::InvalidInput, kind};
}

bool IsExplicitTarget(DeviceSelector const& target) noexcept {
    return target.Kind() != DeviceSelectorKind::Last && target.Kind() != DeviceSelectorKind::Default;
}

std::wstring LowerInvariant(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (const auto character : value) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return lowered;
}

std::wstring DeviceName(auto const& device) {
    if (!device.Alias.empty()) return device.Alias;
    return device.Name.empty() ? device.Id : device.Name;
}

bool IsBusyState(DeviceConnectionState state) noexcept {
    return state == DeviceConnectionState::Connecting || state == DeviceConnectionState::Disconnecting ||
           state == DeviceConnectionState::WaitingForReconnect;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Command Admission and Shutdown ////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppController::CallLease::CallLease(AppController const& owner) noexcept : m_owner(owner) {
    std::scoped_lock lock(m_owner.m_stateMutex);
    if (!m_owner.m_running) return;
    ++m_owner.m_activeCalls;
    m_acquired = true;
}

AppController::CallLease::~CallLease() {
    if (!m_acquired) return;
    std::scoped_lock lock(m_owner.m_stateMutex);
    --m_owner.m_activeCalls;
    if (m_owner.m_activeCalls == 0) m_owner.m_noActiveCalls.notify_all();
}

template <typename Action>
AppResult AppController::WithAdmission(AppCommandKind kind, AppCommandContext context, Action&& action) const noexcept {
    AppResult preflight;
    preflight.Command = kind;

    CallLease lease(*this);
    if (!lease.Acquired()) return MakeFailure(kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
    if (context.IsCancellationRequested()) {
        preflight.Code = AppResultCode::Cancelled;
        if (kind == AppCommandKind::ReconnectAll) preflight.Reason = AppOutcomeReason::NotReady;
        return preflight;
    }
    if (context.IsExpired(AppCommandContext::Clock::now())) {
        preflight.Code = AppResultCode::TimedOut;
        if (kind == AppCommandKind::ReconnectAll) preflight.Reason = AppOutcomeReason::ReconnectFailed;
        return preflight;
    }

    try {
        auto result = action();
        result.Command = kind;
        result.DispatchPhase = AppDispatchPhase::Started;
        return result;
    } catch (...) {
        preflight.DispatchPhase = AppDispatchPhase::Started;
        preflight.Code = AppResultCode::InternalError;
        preflight.Reason = AppOutcomeReason::InternalError;
        return preflight;
    }
}

template <typename Action>
AppResult AppController::WithSettings(AppCommandKind kind, AppCommandContext context, Action&& action) const noexcept {
    return WithAdmission(kind, context, [&] {
        auto const settings = ReadCoherentSettings();
        if (!settings) return MakeFailure(kind, AppResultCode::InternalError, AppOutcomeReason::InternalError);
        return action(*settings);
    });
}

AppSnapshot AppController::Snapshot() const noexcept {
    try {
        CallLease lease(*this);
        if (!lease.Acquired()) {
            AppSnapshot unavailable;
            std::scoped_lock lock(m_stateMutex);
            unavailable.Generation = m_generation;
            unavailable.IsRunning = false;
            return unavailable;
        }
        return CaptureSnapshot();
    } catch (...) {
        AppSnapshot unavailable;
        unavailable.IsRunning = false;
        return unavailable;
    }
}

AppSnapshot AppController::CaptureSnapshot() const {
    for (std::size_t attempt = 0; attempt != 3; ++attempt) {
        auto const settings = ReadSettings();
        if (!settings) break;
        auto const deviceState = m_devices->Snapshot();
        auto const startup = m_startupTask ? std::optional{m_startupTask->Snapshot()} : std::nullopt;
        std::uint64_t generation;
        std::uint64_t pickerGeneration;
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_running) break;
            generation = m_generation;
            pickerGeneration = m_pickerGeneration;
        }
        if (auto presentation = m_presentation.lock())
            pickerGeneration = std::max(pickerGeneration, presentation->PickerOpenedGeneration());
        std::vector<DeviceRecord> inventory;
        for (auto const& device : deviceState.Inventory.Devices)
            inventory.push_back({device.Id, device.Name, {}, DeviceConnectionState::Idle, false, true, false});
        auto records = MergeDevices(std::move(inventory), SessionRecords(deviceState), settings->Data);
        auto snapshot = BuildSnapshot(std::move(records), settings->Data, generation, pickerGeneration, true);
        // Reading each version again establishes an overlapping stable interval
        // for the owner snapshots. Neither owner is called under our mutex.
        if (m_settings->Snapshot().Revision != settings->Revision ||
            m_devices->Snapshot().Generation != deviceState.Generation ||
            (startup && m_startupTask->Snapshot().Publication != startup->Publication))
            continue;
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_running) break;
            if (m_generation != generation || !m_lastSettingsRevision ||
                *m_lastSettingsRevision != settings->Revision ||
                (m_lastDeviceGeneration && *m_lastDeviceGeneration > deviceState.Generation) ||
                (startup && m_lastStartupPublication && *m_lastStartupPublication > startup->Publication))
                continue;
            if (m_lastDeviceGeneration && *m_lastDeviceGeneration != deviceState.Generation)
                AdvanceGeneration(m_generation);
            m_lastDeviceGeneration = deviceState.Generation;
            if (startup) {
                if (m_lastStartupPublication && *m_lastStartupPublication != startup->Publication)
                    AdvanceGeneration(m_generation);
                m_lastStartupPublication = startup->Publication;
            }
            if (pickerGeneration > m_pickerGeneration) {
                m_pickerGeneration = pickerGeneration;
                AdvanceGeneration(m_generation);
            }
            snapshot.Generation = m_generation;
            snapshot.Tray.Generation = m_generation;
        }
        snapshot.StartupTask = startup;
        snapshot.SettingsRevision = settings->Revision;
        snapshot.DeviceGeneration = deviceState.Generation;
        snapshot.InventoryComplete = deviceState.Inventory.EnumerationComplete;
        for (auto& device : snapshot.Devices) {
            device.IsAvailable = device.IsConnected || device.IsBusy ||
                                 std::ranges::any_of(deviceState.Inventory.Devices,
                                                     [&](auto const& known) { return known.Id == device.Id.View(); });
        }
        return snapshot;
    }
    AppSnapshot unavailable;
    unavailable.IsRunning = false;
    return unavailable;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Presentation Actions //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::ShowDevicePicker(DevicePickerOpenMode mode, AppCommandContext context) const noexcept {
    constexpr auto kind = AppCommandKind::ShowDevicePicker;
    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto presentation = m_presentation.lock();
        if (!presentation) return MakeFailure(kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady, input.Data);
        if (const auto code = MutationAdmissionFailure(context))
            return MakeFailure(kind, *code, AppOutcomeReason::NotReady, input.Data);
        return PresentationResult(kind, presentation->PresentDevicePicker(mode, context), input.Data);
    });
}

AppResult AppController::ShowSettings(AppCommandContext context) const noexcept {
    constexpr auto kind = AppCommandKind::ShowSettings;
    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto presentation = m_presentation.lock();
        if (!presentation) return MakeFailure(kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady, input.Data);
        if (const auto code = MutationAdmissionFailure(context))
            return MakeFailure(kind, *code, AppOutcomeReason::NotReady, input.Data);
        return PresentationResult(kind, presentation->PresentSettings(context), input.Data);
    });
}

AppResult AppController::PresentationResult(AppCommandKind kind,
                                            AppUiActionResult const& action,
                                            SettingsData const& settings) const {
    if (action.Status != AppActionStatus::Succeeded) {
        auto const code =
            action.Status == AppActionStatus::Failed ? AppResultCode::Unavailable : ToResultCode(action.Status);
        return MakeFailure(kind, code, AppOutcomeReason::NotReady, settings);
    }
    {
        std::scoped_lock lock(m_stateMutex);
        if (action.DevicePickerOpenedGeneration)
            m_pickerGeneration = std::max(m_pickerGeneration, *action.DevicePickerOpenedGeneration);
        AdvanceGeneration(m_generation);
    }
    AppResult result;
    result.Code = AppResultCode::Success;
    result.Command = kind;
    result.Reason =
        kind == AppCommandKind::ShowDevicePicker ? AppOutcomeReason::ShowOpened : AppOutcomeReason::SettingsOpened;
    result.PrivacyModeEnabled = settings.PrivacyModeEnabled;
    return result;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Queries ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::ListDevices(AppCommandContext context) const noexcept {
    return WithAdmission(AppCommandKind::ListDevices, context, [&] {
        RefreshDevices(context);
        return DeviceQueryResult(CaptureSnapshot());
    });
}

AppResult AppController::Status(AppCommandContext context) const noexcept {
    return WithAdmission(AppCommandKind::Status, context, [&] { return DeviceQueryResult(CaptureSnapshot()); });
}

AppResult AppController::ListAliases(AppCommandContext context) const noexcept {
    return WithAdmission(AppCommandKind::ListAliases, context, [&] { return DeviceQueryResult(CaptureSnapshot()); });
}

AppResult AppController::ShowDefault(AppCommandContext context) const noexcept {
    return WithAdmission(AppCommandKind::ShowDefault, context, [&] { return DeviceQueryResult(CaptureSnapshot()); });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Persistent Device Settings ////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::SetDefault(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::SetDefault;
    if (!IsExplicitTarget(target)) return InvalidInput(kind);
    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);

        auto resolution = Resolve(target, devices, settings);
        if (resolution.Code != AppResultCode::Success) {
            return MakeTargetResult(kind, resolution, settings, resolution.Code, resolution.Reason);
        }
        if (!resolution.HasTarget || !resolution.Target.Exists) {
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::NotFound, AppOutcomeReason::TargetNotFound);
        }

        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        const bool accepted =
            (m_settings->SetDefaultDevice(resolution.Target.Id).Status != SettingsMutationStatus::Rejected);
        if (!accepted) {
            // An external device ID may be longer than the bounded
            // persistence identity. The Store correctly rejects that
            // value, but the legacy control contract reports a resolved
            // live target as the selected default rather than converting
            // the transport-valid request into an operation failure.
            if (!TryDeviceId(resolution.Target.Id)) {
                return MakeTargetResult(
                    kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::DefaultSet);
            }
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::OperationFailed, AppOutcomeReason::InternalError);
        }
        const auto committedSettings = ReadCoherentSettings();
        if (!committedSettings) {
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::InternalError, AppOutcomeReason::InternalError);
        }
        auto committedSnapshot = CaptureSnapshot();
        if (!committedSnapshot.IsRunning)
            return MakeFailure(
                kind, AppResultCode::Indeterminate, AppOutcomeReason::InternalError, committedSettings->Data);
        auto result = MakeTargetResult(
            kind, resolution, committedSettings->Data, AppResultCode::Success, AppOutcomeReason::DefaultSet);
        result.DefaultDevice = committedSnapshot.DefaultDevice;
        result.PrivacyModeEnabled = committedSnapshot.PrivacyModeEnabled;
        return result;
    });
}

AppResult AppController::ClearDefault(AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::ClearDefault;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;

        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeFailure(kind, *code, AppOutcomeReason::None, settings);
        }
        const bool accepted = (m_settings->ClearDefaultDevice().Status != SettingsMutationStatus::Rejected);
        if (!accepted) {
            return MakeFailure(kind, AppResultCode::OperationFailed, AppOutcomeReason::InternalError, settings);
        }
        const auto committedSettings = ReadCoherentSettings();
        if (!committedSettings) {
            return MakeFailure(kind, AppResultCode::InternalError, AppOutcomeReason::InternalError, settings);
        }
        auto committedSnapshot = CaptureSnapshot();
        if (!committedSnapshot.IsRunning)
            return MakeFailure(
                kind, AppResultCode::Indeterminate, AppOutcomeReason::InternalError, committedSettings->Data);
        AppResult result;
        result.Code = AppResultCode::Success;
        result.Command = kind;
        result.Reason = AppOutcomeReason::DefaultCleared;
        result.DefaultDevice = committedSnapshot.DefaultDevice;
        result.PrivacyModeEnabled = committedSnapshot.PrivacyModeEnabled;
        return result;
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Bulk Device Actions ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::DisconnectAll(AppCommandContext context) const noexcept {
    constexpr auto kind = AppCommandKind::DisconnectAll;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;

        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeFailure(kind, *code, AppOutcomeReason::None, settings);
        }
        (void)m_devices->DisconnectAll();
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        AppResult result;
        result.Code = AppResultCode::Success;
        result.Command = kind;
        result.Reason = AppOutcomeReason::DisconnectAllSucceeded;
        result.PrivacyModeEnabled = PrivacyMode(settings);
        return result;
    });
}

AppResult AppController::ReconnectAll(AppCommandContext context) const noexcept {
    constexpr auto kind = AppCommandKind::ReconnectAll;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevicesWithoutRefresh(settings);

        if (context.Completion == AppCommandContext::CompletionMode::Detached) {
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeFailure(kind, *code, AppOutcomeReason::None, settings);
            }
            (void)m_devices->ReconnectAll();
            {
                std::scoped_lock lock(m_stateMutex);
                AdvanceGeneration(m_generation);
            }
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = kind;
            result.Reason = AppOutcomeReason::ReconnectAllSucceeded;
            result.PrivacyModeEnabled = PrivacyMode(settings);
            return result;
        }

        for (auto const& device : devices) {
            if (!device.IsConnected || device.Id.empty()) continue;
            auto selector = DeviceSelector::ById(device.Id);
            if (!selector) continue;
            auto resolution = Resolve(*selector, devices, settings);
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeTargetResult(kind,
                                        resolution,
                                        settings,
                                        *code,
                                        *code == AppResultCode::Cancelled ? AppOutcomeReason::NotReady
                                                                          : AppOutcomeReason::ReconnectFailed);
            }
            const auto operation = PerformDeviceOperation(AppCommandKind::Reconnect, device.Id, context);
            if (!IsSuccess(operation.Status)) {
                const auto code = operation.Status == OperationStatus::Failed ? AppResultCode::Indeterminate
                                                                              : ToResultCode(operation.Status);
                const auto reason = operation.Status == OperationStatus::Cancelled ? AppOutcomeReason::NotReady
                                                                                   : AppOutcomeReason::ReconnectFailed;
                return MakeTargetResult(kind, resolution, settings, code, reason);
            }
            auto after = BuildDevicesWithoutRefresh(settings);
            auto current = FindById(after, device.Id);
            if (!current || !current->IsConnected) {
                return MakeTargetResult(
                    kind, resolution, settings, AppResultCode::OperationFailed, AppOutcomeReason::ReconnectFailed);
            }
        }
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        AppResult result;
        result.Code = AppResultCode::Success;
        result.Command = kind;
        result.Reason = AppOutcomeReason::ReconnectAllSucceeded;
        result.PrivacyModeEnabled = PrivacyMode(settings);
        return result;
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Alias Actions /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::SetAlias(DeviceSelector target, std::wstring_view alias, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::SetAlias;
    if (!IsExplicitTarget(target) || alias.empty() || alias.size() > c_maxAppCommandTextCharacters ||
        alias.find_first_of(std::wstring_view{L"\r\n\0", 3}) != std::wstring_view::npos)
        return InvalidInput(kind);
    return WriteAlias(kind, target, alias, context);
}

AppResult AppController::ClearAlias(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::ClearAlias;
    if (!IsExplicitTarget(target)) return InvalidInput(kind);
    return WriteAlias(kind, target, {}, context);
}

AppResult AppController::WriteAlias(AppCommandKind kind,
                                    DeviceSelector const& target,
                                    std::wstring_view alias,
                                    AppCommandContext context) const {
    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);

        auto resolution = Resolve(target, devices, settings);
        if (resolution.Code != AppResultCode::Success) {
            return MakeTargetResult(kind, resolution, settings, resolution.Code, resolution.Reason);
        }
        if (!resolution.HasTarget || !resolution.Target.Exists) {
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::NotFound, AppOutcomeReason::TargetNotFound);
        }

        if (!apc::limits::IsBoundedUtf16(alias, apc::limits::c_maxDeviceAliasCharacters)) {
            return MakeTargetResult(kind,
                                    resolution,
                                    settings,
                                    AppResultCode::OperationFailed,
                                    kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSetFailed
                                                                     : AppOutcomeReason::AliasClearFailed);
        }
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        const bool accepted =
            (m_settings->SetDeviceAlias(resolution.Target.Id, alias, resolution.Target.Name).Mutation.Status !=
             SettingsMutationStatus::Rejected);
        if (!accepted) {
            return MakeTargetResult(kind,
                                    resolution,
                                    settings,
                                    AppResultCode::OperationFailed,
                                    kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSetFailed
                                                                     : AppOutcomeReason::AliasClearFailed);
        }

        const auto committedSettings = ReadCoherentSettings();
        if (!committedSettings) {
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::InternalError, AppOutcomeReason::InternalError);
        }
        auto refreshedDevices = BuildDevicesWithoutRefresh(committedSettings->Data);
        auto result = MakeTargetResult(kind,
                                       resolution,
                                       committedSettings->Data,
                                       AppResultCode::Success,
                                       kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSet
                                                                        : AppOutcomeReason::AliasCleared);
        const auto committedDevice =
            std::ranges::find_if(committedSettings->Data.Devices, [&resolution](auto const& device) {
                return EqualsIgnoreCase(device.Id, resolution.Target.Id);
            });
        result.Alias =
            committedDevice == committedSettings->Data.Devices.end() ? std::wstring{} : committedDevice->Alias;
        result.Device = PostOperationDevice(resolution.Target.Id, refreshedDevices);
        return result;
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Connection Actions ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::Connect(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::Connect;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);
        return RunDeviceOperation(kind, target, context, devices, settings);
    });
}

AppResult AppController::Disconnect(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::Disconnect;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);
        return RunDeviceOperation(kind, target, context, devices, settings);
    });
}

AppResult AppController::Reconnect(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::Reconnect;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);
        return RunDeviceOperation(kind, target, context, devices, settings);
    });
}

AppResult AppController::Toggle(DeviceSelector target, AppCommandContext context) const {
    constexpr auto kind = AppCommandKind::ToggleLast;

    return WithSettings(kind, context, [&](SettingsSnapshot const& input) {
        auto const& settings = input.Data;
        auto devices = BuildDevices(IsRefreshNeeded(target.Kind()), context, settings);

        auto resolution = Resolve(target, devices, settings);
        if (resolution.Code != AppResultCode::Success) {
            return MakeTargetResult(kind, resolution, settings, resolution.Code, resolution.Reason);
        }
        if (context.Completion == AppCommandContext::CompletionMode::Detached) {
            const bool globalBusy = m_devices->HasBusyOperations();
            const bool deviceBusy = m_devices->IsDeviceBusy(resolution.Target.Id);
            if (globalBusy || deviceBusy) {
                return MakeTargetResult(kind, resolution, settings, AppResultCode::Busy, AppOutcomeReason::None);
            }
        }

        auto result =
            RunDeviceOperation(resolution.Target.IsConnected ? AppCommandKind::Disconnect : AppCommandKind::Connect,
                               target,
                               context,
                               devices,
                               settings);
        result.Command = kind;
        return result;
    });
}

AppResult AppController::ToggleDefault(AppCommandContext context) const {
    return Toggle(DeviceSelector::Default(), context);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Exact Identity Actions ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::SetDefault(std::wstring_view deviceId) const {
    auto target = DeviceSelector::ById(deviceId);
    return target ? SetDefault(std::move(*target), {}) : InvalidInput(AppCommandKind::SetDefault);
}

AppResult AppController::SetAlias(std::wstring_view deviceId, std::wstring_view alias) const {
    if (alias.empty()) return ClearAlias(deviceId);
    auto target = DeviceSelector::ById(deviceId);
    return target ? SetAlias(std::move(*target), alias, {}) : InvalidInput(AppCommandKind::SetAlias);
}

AppResult AppController::ClearAlias(std::wstring_view deviceId) const {
    auto target = DeviceSelector::ById(deviceId);
    return target ? ClearAlias(std::move(*target), {}) : InvalidInput(AppCommandKind::ClearAlias);
}

AppResult AppController::RunDeviceOperation(AppCommandKind kind,
                                            DeviceSelector const& target,
                                            AppCommandContext const& context,
                                            std::vector<DeviceRecord> const& devices,
                                            SettingsData const& settings) const {
    auto resolution = Resolve(target, devices, settings);
    if (resolution.Code != AppResultCode::Success) {
        return MakeTargetResult(kind, resolution, settings, resolution.Code, resolution.Reason);
    }

    const auto id = resolution.Target.Id;
    if (kind == AppCommandKind::Connect && resolution.Target.IsConnected) {
        return MakeTargetResult(kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::AlreadyConnected);
    }
    if (kind == AppCommandKind::Disconnect && !resolution.Target.IsConnected) {
        return MakeTargetResult(
            kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::AlreadyDisconnected);
    }

    const bool detached = context.Completion == AppCommandContext::CompletionMode::Detached;
    if (kind == AppCommandKind::Disconnect) {
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        (void)m_devices->Disconnect(id);
        auto after = BuildDevicesWithoutRefresh(settings);
        auto current = FindById(after, id);
        if (current && current->IsConnected) {
            return MakeTargetResult(
                kind, resolution, settings, AppResultCode::OperationFailed, AppOutcomeReason::DisconnectFailed);
        }
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        auto result =
            MakeTargetResult(kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::DisconnectSucceeded);
        result.Device = PostOperationDevice(id, after);
        return result;
    }

    if (detached) {
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        auto const admitted = kind == AppCommandKind::Connect ? m_devices->Connect(id) : m_devices->Reconnect(id);
        if (admitted.Kind == apc::device::DeviceCommandResultKind::Rejected)
            return MakeTargetResult(kind, resolution, settings, AppResultCode::Busy, AppOutcomeReason::None);
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        return MakeTargetResult(kind,
                                resolution,
                                settings,
                                AppResultCode::Success,
                                kind == AppCommandKind::Connect ? AppOutcomeReason::ConnectSucceeded
                                                                : AppOutcomeReason::ReconnectSucceeded);
    }

    if (const auto code = MutationAdmissionFailure(context)) {
        return MakeTargetResult(kind, resolution, settings, *code, AppOutcomeReason::None);
    }
    const auto operationResult = PerformDeviceOperation(kind, id, context);
    if (!IsSuccess(operationResult.Status)) {
        const auto code = operationResult.Status == OperationStatus::Failed ? AppResultCode::Indeterminate
                                                                            : ToResultCode(operationResult.Status);
        return MakeTargetResult(kind, resolution, settings, code, AppOutcomeReason::None);
    }

    auto after = BuildDevicesWithoutRefresh(settings);
    auto current = FindById(after, id);
    if (!current || !current->IsConnected) {
        return MakeTargetResult(kind, resolution, settings, AppResultCode::OperationFailed, OperationReason(kind));
    }

    {
        std::scoped_lock lock(m_stateMutex);
        AdvanceGeneration(m_generation);
    }
    auto result = MakeTargetResult(kind,
                                   resolution,
                                   settings,
                                   AppResultCode::Success,
                                   kind == AppCommandKind::Connect ? AppOutcomeReason::ConnectSucceeded
                                                                   : AppOutcomeReason::ReconnectSucceeded);
    result.Device = PostOperationDevice(id, after);
    return result;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Target Resolution /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppController::Resolution AppController::Resolve(DeviceSelector const& selector,
                                                 std::vector<DeviceRecord> const& devices,
                                                 SettingsData const& settings) const {
    Resolution result;
    const auto makeResolved = [&](DeviceRecord const& device) {
        result.HasTarget = true;
        result.Device = device;
        result.Target = *ToTarget(device);
    };
    const auto makeUnknown = [&](std::wstring id) {
        result.HasTarget = true;
        result.Target = AppTargetSnapshot{std::move(id), {}, {}, {}, false, false, false};
        result.Target.DisplayName = result.Target.Id;
    };
    const auto matchOne = [&](std::vector<DeviceRecord> matches, std::wstring_view query) {
        std::ranges::sort(matches, [](auto const& left, auto const& right) { return left.Id < right.Id; });
        const auto uniqueEnd =
            std::ranges::unique(matches, [](auto const& left, auto const& right) { return left.Id == right.Id; });
        matches.resize(static_cast<std::size_t>(uniqueEnd.begin() - matches.begin()));
        if (matches.empty()) {
            result.Code = AppResultCode::NotFound;
            result.Reason = AppOutcomeReason::TargetNotFound;
            result.RequestedTarget = query;
            return;
        }
        if (matches.size() != 1) {
            result.Code = AppResultCode::Ambiguous;
            result.Reason = AppOutcomeReason::TargetAmbiguous;
            result.RequestedTarget = query;
            return;
        }
        makeResolved(matches.front());
    };

    if (selector.Kind() == DeviceSelectorKind::Id) {
        const auto id = std::wstring(selector.IdText());
        if (auto device = FindById(devices, id)) {
            makeResolved(*device);
        } else {
            makeUnknown(id);
        }
        return result;
    }

    if (selector.Kind() == DeviceSelectorKind::Default || selector.Kind() == DeviceSelectorKind::Last) {
        std::wstring id;
        if (selector.Kind() == DeviceSelectorKind::Default &&
            settings.DefaultDevice == ::DefaultDeviceMode::SpecificDevice && !settings.DefaultDeviceId.empty()) {
            id = settings.DefaultDeviceId;
        } else if (!settings.LastConnectedIds.empty()) {
            id = settings.LastConnectedIds.front();
        } else {
            result.Code = AppResultCode::NotFound;
            result.Reason = selector.Kind() == DeviceSelectorKind::Default ? AppOutcomeReason::DefaultTargetMissing
                                                                           : AppOutcomeReason::LastTargetMissing;
            return result;
        }
        if (auto device = FindById(devices, id)) {
            makeResolved(*device);
        } else {
            makeUnknown(std::move(id));
        }
        return result;
    }

    const auto query = selector.Query();
    if (selector.Kind() == DeviceSelectorKind::Name) {
        std::vector<DeviceRecord> matches;
        for (auto const& device : devices) {
            if (EqualsIgnoreCase(device.Name, query)) matches.push_back(device);
        }
        if (matches.empty()) {
            for (auto const& device : devices) {
                if (ContainsIgnoreCase(device.Name, query)) matches.push_back(device);
            }
        }
        matchOne(std::move(matches), query);
        return result;
    }
    if (selector.Kind() == DeviceSelectorKind::Mac) {
        const auto normalized = NormalizeHex(query);
        std::vector<DeviceRecord> matches;
        if (normalized.size() >= 6) {
            for (auto const& device : devices) {
                if (NormalizeHex(device.Id).find(normalized) != std::wstring::npos) matches.push_back(device);
            }
        }
        matchOne(std::move(matches), query);
        return result;
    }
    if (selector.Kind() == DeviceSelectorKind::Alias) {
        std::vector<DeviceRecord> matches;
        for (auto const& device : devices) {
            if (EqualsIgnoreCase(device.Alias, query)) matches.push_back(device);
        }
        if (matches.empty()) {
            for (auto const& device : devices) {
                if (ContainsIgnoreCase(device.Alias, query)) matches.push_back(device);
            }
        }
        matchOne(std::move(matches), query);
        return result;
    }
    if (selector.Kind() == DeviceSelectorKind::Auto) {
        std::vector<DeviceRecord> matches;
        for (auto const& device : devices) {
            if (EqualsIgnoreCase(device.Id, query)) matches.push_back(device);
        }
        if (matches.empty()) {
            for (auto const& device : devices) {
                if (EqualsIgnoreCase(device.Alias, query) || EqualsIgnoreCase(device.Name, query)) {
                    matches.push_back(device);
                }
            }
        }
        if (matches.empty()) {
            const auto normalized = NormalizeHex(query);
            if (normalized.size() >= 6) {
                for (auto const& device : devices) {
                    if (NormalizeHex(device.Id).find(normalized) != std::wstring::npos) matches.push_back(device);
                }
            }
        }
        if (matches.empty()) {
            for (auto const& device : devices) {
                if (ContainsIgnoreCase(device.Alias, query) || ContainsIgnoreCase(device.Name, query)) {
                    matches.push_back(device);
                }
            }
        }
        matchOne(std::move(matches), query);
        return result;
    }

    result.Code = AppResultCode::InvalidInput;
    result.Reason = AppOutcomeReason::TargetRequired;
    return result;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Owner Snapshots ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::vector<AppController::DeviceRecord>
AppController::BuildDevices(bool refresh, AppCommandContext const& context, SettingsData const& settings) const {
    if (refresh) RefreshDevices(context);
    return BuildDevicesWithoutRefresh(settings);
}

void AppController::RefreshDevices(AppCommandContext const& context) const {
    try {
        auto const refreshContext = CappedRefreshContext(context);
        auto operation = m_devices->RefreshDevicesAsync();
        struct Completion {
            std::mutex Mutex;
            std::condition_variable_any Changed;
            bool Done = false;
        };
        auto completion = std::make_shared<Completion>();
        operation.Completed([completion](auto const&, auto const&) {
            {
                std::lock_guard lock(completion->Mutex);
                completion->Done = true;
            }
            completion->Changed.notify_all();
        });
        std::unique_lock lock(completion->Mutex);
        const bool completed = completion->Changed.wait_until(
            lock, refreshContext.StopToken, refreshContext.Deadline, [&] { return completion->Done; });
        lock.unlock();
        if (!completed) operation.Cancel();
    } catch (winrt::hresult_error const&) {
        // Discovery is optional; retain the device owner's known inventory.
        // Mutations recheck the caller's context after resolution.
    }
}

std::vector<AppController::DeviceRecord> AppController::BuildDevicesWithoutRefresh(SettingsData const& settings) const {
    auto const snapshot = m_devices->Snapshot();
    std::vector<DeviceRecord> inventory;
    for (auto const& device : snapshot.Inventory.Devices)
        inventory.push_back({device.Id, device.Name, {}, DeviceConnectionState::Idle, false, true, false});
    return MergeDevices(std::move(inventory), SessionRecords(snapshot), settings);
}

std::optional<SettingsSnapshot> AppController::ReadSettings() const noexcept {
    try {
        auto snapshot = m_settings->Snapshot();
        // Only the Store revision is retained. SettingsData
        // remains a caller-local value and is never cached here.
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_lastSettingsRevision) {
                m_lastSettingsRevision = snapshot.Revision;
            } else if (snapshot.Revision > *m_lastSettingsRevision) {
                m_lastSettingsRevision = snapshot.Revision;
                AdvanceGeneration(m_generation);
            }
        }
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SettingsSnapshot> AppController::ReadCoherentSettings() const noexcept {
    // SettingsStore snapshots are monotonic. A stale callback completion can
    // only be reconciled by taking another value snapshot; never cache its
    // SettingsData in the controller or hold its mutex while invoking Store.
    for (std::size_t attempt = 0; attempt != 3; ++attempt) {
        const auto snapshot = ReadSettings();
        if (!snapshot) return std::nullopt;

        std::scoped_lock lock(m_stateMutex);
        if (m_lastSettingsRevision && snapshot->Revision == *m_lastSettingsRevision) return snapshot;
    }
    return std::nullopt;
}

std::vector<AppController::DeviceRecord>
AppController::SessionRecords(apc::device::DeviceServiceSnapshot const& snapshot) {
    std::vector<DeviceRecord> records;
    for (auto const& session : snapshot.Sessions) {
        DeviceConnectionState state = DeviceConnectionState::Idle;
        switch (session.State) {
            case apc::device::DeviceLifecycleState::Idle: break;
            case apc::device::DeviceLifecycleState::Connecting: state = DeviceConnectionState::Connecting; break;
            case apc::device::DeviceLifecycleState::Disconnecting: state = DeviceConnectionState::Disconnecting; break;
            case apc::device::DeviceLifecycleState::Connected: state = DeviceConnectionState::Connected; break;
            case apc::device::DeviceLifecycleState::WaitingForReconnect:
                state = DeviceConnectionState::WaitingForReconnect;
                break;
            case apc::device::DeviceLifecycleState::Failed: state = DeviceConnectionState::Failed; break;
        }
        records.push_back({session.DeviceId,
                           session.DeviceName,
                           {},
                           state,
                           state == DeviceConnectionState::Connected,
                           true,
                           IsBusyState(state)});
    }
    return records;
}

std::vector<AppController::DeviceRecord> AppController::MergeDevices(std::vector<DeviceRecord> refreshed,
                                                                     std::vector<DeviceRecord> connected,
                                                                     SettingsData const& settings) const {
    std::unordered_map<std::wstring, std::size_t> indexes;
    std::vector<DeviceRecord> merged;
    const auto upsert = [&](DeviceRecord device) {
        if (device.Id.empty()) return;
        auto [entry, inserted] = indexes.emplace(device.Id, merged.size());
        if (inserted) {
            merged.push_back(std::move(device));
            return;
        }
        auto& current = merged[entry->second];
        if (!device.Name.empty()) current.Name = std::move(device.Name);
        if (!device.Alias.empty()) current.Alias = std::move(device.Alias);
        current.IsConnected = current.IsConnected || device.IsConnected;
        current.IsKnown = current.IsKnown || device.IsKnown;
        current.IsBusy = current.IsBusy || device.IsBusy;
        if (current.State == DeviceConnectionState::Idle && device.State != DeviceConnectionState::Idle) {
            current.State = device.State;
        }
    };

    for (auto& device : refreshed)
        upsert(std::move(device));
    // Keep session state available until the presentation labels have been merged.
    for (auto const& device : connected)
        upsert(device);
    for (auto const& device : settings.Devices) {
        upsert(DeviceRecord{device.Id, device.Name, device.Alias, DeviceConnectionState::Idle, false, true, false});
    }

    ApplySessionStates(merged, connected);
    std::ranges::sort(merged, [](auto const& left, auto const& right) {
        const auto leftLabel = LowerInvariant(DeviceLabel(left));
        const auto rightLabel = LowerInvariant(DeviceLabel(right));
        if (leftLabel != rightLabel) return leftLabel < rightLabel;
        return LowerInvariant(left.Id) < LowerInvariant(right.Id);
    });
    return merged;
}

AppSnapshot AppController::BuildSnapshot(std::vector<DeviceRecord> devices,
                                         SettingsData const& settings,
                                         std::uint64_t generation,
                                         std::uint64_t pickerGeneration,
                                         bool isRunning) const {
    AppSnapshot snapshot;
    snapshot.Generation = generation;
    snapshot.IsRunning = isRunning;
    snapshot.PrivacyModeEnabled = settings.PrivacyModeEnabled;
    snapshot.Settings = settings;
    for (auto const& device : devices) {
        if (auto value = ToSnapshot(device)) snapshot.Devices.push_back(std::move(*value));
    }
    for (auto const& id : settings.LastConnectedIds) {
        if (auto value = TryDeviceId(id)) snapshot.LastConnectedDeviceIds.push_back(std::move(*value));
    }

    const auto resolveDefault = [&](std::wstring const& id) -> DefaultDeviceSnapshot {
        DefaultDeviceSnapshot value;
        value.Mode = settings.DefaultDevice == ::DefaultDeviceMode::SpecificDevice
                         ? apc::app::DefaultDeviceMode::SpecificDevice
                         : apc::app::DefaultDeviceMode::LastConnected;
        if (auto internalId = TryDeviceId(id)) value.Id = *internalId;
        if (auto found = FindById(devices, id)) {
            value.DisplayName = DeviceName(*found);
            value.IsResolved = true;
            value.IsConnected = found->IsConnected;
        } else {
            value.DisplayName = id;
        }
        return value;
    };

    DefaultDeviceSnapshot defaultDevice;
    defaultDevice.Mode = settings.DefaultDevice == ::DefaultDeviceMode::SpecificDevice
                             ? apc::app::DefaultDeviceMode::SpecificDevice
                             : apc::app::DefaultDeviceMode::LastConnected;
    if (settings.DefaultDevice == ::DefaultDeviceMode::SpecificDevice && !settings.DefaultDeviceId.empty()) {
        defaultDevice = resolveDefault(settings.DefaultDeviceId);
    } else if (!settings.LastConnectedIds.empty()) {
        defaultDevice = resolveDefault(settings.LastConnectedIds.front());
        defaultDevice.Mode = apc::app::DefaultDeviceMode::LastConnected;
    }
    snapshot.DefaultDevice = std::move(defaultDevice);

    snapshot.Tray.Generation = generation;
    snapshot.Tray.DevicePickerOpenedGeneration = pickerGeneration;
    snapshot.Tray.HasBusyOperations = std::ranges::any_of(devices, [](auto const& device) { return device.IsBusy; });
    for (auto const& device : snapshot.Devices) {
        if (device.IsConnected) snapshot.Tray.ConnectedDevices.push_back(device);
    }
    if (auto presentation = m_presentation.lock()) {
        try {
            snapshot.AdaptiveResources = presentation->ResourceStatus();
        } catch (...) {
        }
    }
    return snapshot;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Result Projection /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::DeviceQueryResult(AppSnapshot snapshot) const {
    AppResult result;
    if (!snapshot.IsRunning) {
        result.Code = AppResultCode::Unavailable;
        result.Reason = AppOutcomeReason::NotReady;
        return result;
    }
    result.Devices = snapshot.Devices;
    result.DefaultDevice = snapshot.DefaultDevice;
    result.PrivacyModeEnabled = snapshot.PrivacyModeEnabled;
    result.Snapshot = std::move(snapshot);
    return result;
}

AppResult AppController::MakeFailure(AppCommandKind command,
                                     AppResultCode code,
                                     AppOutcomeReason reason,
                                     std::wstring requestedTarget) const {
    AppResult result;
    result.Code = code;
    result.Command = command;
    result.Reason = reason;
    result.RequestedTarget = std::move(requestedTarget);
    return result;
}

AppResult AppController::MakeFailure(AppCommandKind command,
                                     AppResultCode code,
                                     AppOutcomeReason reason,
                                     SettingsData const& settings,
                                     std::wstring requestedTarget) const {
    auto result = MakeFailure(command, code, reason, std::move(requestedTarget));
    result.PrivacyModeEnabled = PrivacyMode(settings);
    return result;
}

AppResult AppController::MakeTargetResult(AppCommandKind command,
                                          Resolution const& resolution,
                                          SettingsData const& settings,
                                          AppResultCode code,
                                          AppOutcomeReason reason) const {
    auto result = MakeFailure(command, code, reason, settings, resolution.RequestedTarget);
    if (resolution.HasTarget) result.Target = resolution.Target;
    if (resolution.Device) result.Device = ToSnapshot(*resolution.Device);
    return result;
}

std::optional<DeviceSnapshot> AppController::ToSnapshot(DeviceRecord const& record) const {
    auto id = ExternalDeviceId::TryCreate(record.Id);
    if (!id) return std::nullopt;
    return DeviceSnapshot{std::move(*id),
                          record.Name,
                          record.Alias,
                          DeviceLabel(record),
                          record.State,
                          record.IsKnown,
                          record.IsConnected,
                          record.IsBusy || IsBusyState(record.State)};
}

std::optional<AppTargetSnapshot> AppController::ToTarget(DeviceRecord const& record) const {
    return AppTargetSnapshot{
        record.Id, record.Name, record.Alias, DeviceLabel(record), true, record.IsConnected, record.IsKnown};
}

std::optional<DeviceSnapshot> AppController::PostOperationDevice(std::wstring_view deviceId,
                                                                 std::vector<DeviceRecord> const& devices) const {
    if (auto record = FindById(devices, deviceId)) return ToSnapshot(*record);
    return std::nullopt;
}

std::wstring AppController::DeviceLabel(DeviceRecord const& device) {
    return DeviceName(device);
}

bool AppController::EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return LowerInvariant(left) == LowerInvariant(right);
}

bool AppController::ContainsIgnoreCase(std::wstring_view value, std::wstring_view query) {
    return !query.empty() && LowerInvariant(value).find(LowerInvariant(query)) != std::wstring::npos;
}

std::wstring AppController::NormalizeHex(std::wstring_view value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (const auto character : value) {
        if ((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F')) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return normalized;
}

std::optional<AppController::DeviceRecord> AppController::FindById(std::vector<DeviceRecord> const& devices,
                                                                   std::wstring_view id) {
    auto found = std::ranges::find_if(devices, [id](auto const& device) { return EqualsIgnoreCase(device.Id, id); });
    if (found == devices.end()) return std::nullopt;
    return *found;
}

std::optional<apc::core::DeviceId> AppController::TryDeviceId(std::wstring_view id) {
    return apc::core::DeviceId::TryCreate(id);
}

AppResultCode AppController::ToResultCode(OperationStatus status) noexcept {
    switch (status) {
        case OperationStatus::Succeeded: return AppResultCode::Success;
        case OperationStatus::Failed: return AppResultCode::OperationFailed;
        case OperationStatus::Cancelled: return AppResultCode::Cancelled;
        case OperationStatus::TimedOut: return AppResultCode::TimedOut;
        case OperationStatus::Indeterminate: return AppResultCode::Indeterminate;
    }
    return AppResultCode::InternalError;
}

AppOutcomeReason AppController::OperationReason(AppCommandKind command) noexcept {
    switch (command) {
        case AppCommandKind::Connect: return AppOutcomeReason::ConnectFailed;
        case AppCommandKind::Disconnect: return AppOutcomeReason::DisconnectFailed;
        case AppCommandKind::Reconnect:
        case AppCommandKind::ReconnectAll: return AppOutcomeReason::ReconnectFailed;
        default: return AppOutcomeReason::InternalError;
    }
}

bool AppController::IsSuccess(OperationStatus status) noexcept {
    return status == OperationStatus::Succeeded;
}

std::optional<AppResultCode> AppController::MutationAdmissionFailure(AppCommandContext const& context) noexcept {
    if (context.IsCancellationRequested()) return AppResultCode::Cancelled;
    if (context.IsExpired(AppCommandContext::Clock::now())) return AppResultCode::TimedOut;
    return std::nullopt;
}

void AppController::ApplySessionStates(std::vector<DeviceRecord>& devices,
                                       std::vector<DeviceRecord> const& connectedDevices) const {
    for (auto& current : devices) {
        const auto source = FindById(connectedDevices, current.Id);
        // The session map is authoritative for whether a device is connected.
        // Facts cross the UI boundary asynchronously, so a late Connected fact
        // must not revive a source record that has already been closed.
        if (source) {
            current.State = source->State;
            current.IsConnected = source->IsConnected;
            current.IsKnown = current.IsKnown || source->IsKnown;
            current.IsBusy = source->IsBusy;
            continue;
        }

        current.IsConnected = false;
        current.State = DeviceConnectionState::Idle;
        current.IsBusy = false;
    }
}

bool AppController::IsRefreshNeeded(DeviceSelectorKind selectorKind) noexcept {
    return selectorKind == DeviceSelectorKind::Name || selectorKind == DeviceSelectorKind::Mac ||
           selectorKind == DeviceSelectorKind::Auto;
}

AppCommandContext AppController::CappedRefreshContext(AppCommandContext const& context) {
    auto capped = context;
    const auto now = AppCommandContext::Clock::now();
    const auto localDeadline = now + c_refreshTimeout;
    if (capped.Deadline == AppCommandContext::TimePoint::max() || capped.Deadline > localDeadline) {
        capped.Deadline = localDeadline;
    }
    return capped;
}

void AppController::AdvanceGeneration(std::uint64_t& generation) noexcept {
    if (generation != std::numeric_limits<std::uint64_t>::max()) ++generation;
}

bool AppController::PrivacyMode(SettingsData const& settings) const noexcept {
    return settings.PrivacyModeEnabled;
}

AppController::OperationResult AppController::PerformDeviceOperation(AppCommandKind command,
                                                                     std::wstring_view id,
                                                                     AppCommandContext const& context) const {
    auto const admission = command == AppCommandKind::Connect ? m_devices->Connect(std::wstring(id))
                                                              : m_devices->Reconnect(std::wstring(id));
    switch (m_devices->WaitForCompletion(admission, context.StopToken, context.Deadline)) {
        case apc::device::DeviceOperationStatus::Succeeded: return {OperationStatus::Succeeded};
        case apc::device::DeviceOperationStatus::Cancelled: return {OperationStatus::Cancelled};
        case apc::device::DeviceOperationStatus::TimedOut: return {OperationStatus::TimedOut};
        case apc::device::DeviceOperationStatus::Failed:
        case apc::device::DeviceOperationStatus::Rejected: return {OperationStatus::Failed};
    }
    return {OperationStatus::Failed};
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings Actions //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

template <typename Mutation> SettingsMutationResult AppController::MutateSettings(Mutation&& mutation) const noexcept {
    CallLease lease(*this);
    if (!lease.Acquired()) return {SettingsMutationStatus::Rejected, 0};
    try {
        return mutation();
    } catch (...) {
        return {SettingsMutationStatus::Rejected, 0};
    }
}

AppController::StartupTaskRequestResult AppController::RefreshStartupTask() const noexcept {
    CallLease lease(*this);
    if (!lease.Acquired() || !m_startupTask) return StartupTaskRequestResult::Unavailable;
    return m_startupTask->Refresh() ? StartupTaskRequestResult::Accepted : StartupTaskRequestResult::Unavailable;
}

AppController::StartupTaskRequestResult AppController::SetStartWithWindows(bool enabled) const noexcept {
    CallLease lease(*this);
    if (!lease.Acquired() || !m_startupTask) return StartupTaskRequestResult::Unavailable;
    return m_startupTask->RequestDesired(enabled) ? StartupTaskRequestResult::Accepted
                                                  : StartupTaskRequestResult::Unavailable;
}

SettingsMutationResult AppController::SetGlobalConnectOnStartup(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetGlobalConnectOnStartup(enabled); });
}

SettingsMutationResult AppController::SetGlobalReconnectOnConnectionLoss(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetGlobalReconnectOnConnectionLoss(enabled); });
}

SettingsMutationResult AppController::SetAllowIncomingConnections(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetAllowIncomingConnections(enabled); });
}

SettingsMutationResult AppController::SetShowNotifications(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetShowNotifications(enabled); });
}

SettingsMutationResult AppController::SetSystemBackdropEffects(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetUseSystemBackdropEffects(enabled); });
}

SettingsMutationResult AppController::SetPrivacyMode(bool enabled) const {
    return MutateSettings([&] { return m_settings->SetPrivacyModeEnabled(enabled); });
}

SettingsMutationResult AppController::SetLanguage(std::wstring language) const {
    if (language.empty()) language = L"system";
    return MutateSettings([&] { return m_settings->SetLanguage(language); });
}

SettingsMutationResult AppController::SetSettingsWindowBounds(PersistedWindowBounds bounds) const {
    return MutateSettings([&] { return m_settings->SetSettingsWindowBounds(bounds); });
}

SettingsMutationResult AppController::ClearSettingsWindowBounds() const {
    return MutateSettings([&] { return m_settings->SetSettingsWindowBounds(std::nullopt); });
}

SettingsMutationResult AppController::SetDeviceConnectOnStartup(std::wstring const& id, bool enabled) const {
    return MutateSettings([&] {
        if (!RememberKnownDevice(id)) return SettingsMutationResult{SettingsMutationStatus::Rejected, 0};
        return m_settings->SetDeviceConnectOnStartup(id, enabled);
    });
}

SettingsMutationResult AppController::SetDeviceReconnectOnConnectionLoss(std::wstring const& id, bool enabled) const {
    return MutateSettings([&] {
        if (!RememberKnownDevice(id)) return SettingsMutationResult{SettingsMutationStatus::Rejected, 0};
        return m_settings->SetDeviceReconnectOnConnectionLoss(id, enabled);
    });
}

SettingsMutationResult AppController::ForgetDevice(std::wstring const& id) const {
    return MutateSettings([&] { return m_settings->ForgetDevice(id); });
}
bool AppController::RememberKnownDevice(std::wstring const& id) const {
    if (!apc::core::DeviceId::TryCreate(id)) return false;
    auto settings = m_settings->Snapshot();
    if (std::ranges::find(settings.Data.Devices, id, &DeviceSettings::Id) != settings.Data.Devices.end()) return true;
    auto devices = m_devices->Snapshot();
    auto found = std::ranges::find(devices.Inventory.Devices, id, &apc::device_picker::DeviceIdentity::Id);
    if (found == devices.Inventory.Devices.end()) return false;
    auto result =
        m_settings->RememberDevice(id, apc::limits::TruncateUtf16(found->Name, apc::limits::c_maxDeviceNameCharacters));
    return result.Status != SettingsMutationStatus::Rejected;
}

} // namespace apc::app
