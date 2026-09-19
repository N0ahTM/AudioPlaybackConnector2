#include <app/AppController.hpp>
#include <core/DeviceService.hpp>
#include <winrt/Windows.Foundation.Collections.h>

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

AppResult AppController::Execute(AppCommand const& command, AppCommandContext context) const noexcept {
    AppResult preflight;
    preflight.Command = command.Kind;
    if (!command.IsWellFormed()) {
        preflight.Code = AppResultCode::InvalidInput;
        return preflight;
    }
    CallLease lease(*this);
    if (!lease.Acquired()) return MakeFailure(command.Kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
    if (context.IsCancellationRequested()) {
        preflight.Code = AppResultCode::Cancelled;
        if (command.Kind == AppCommandKind::ReconnectAll) preflight.Reason = AppOutcomeReason::NotReady;
        return preflight;
    }
    if (context.IsExpired(AppCommandContext::Clock::now())) {
        preflight.Code = AppResultCode::TimedOut;
        if (command.Kind == AppCommandKind::ReconnectAll) preflight.Reason = AppOutcomeReason::ReconnectFailed;
        return preflight;
    }

    try {
        // Only read-only queries can be replayed after a newer Store revision
        // overtakes their input. Commands with side effects run once; their
        // post-mutation paths explicitly reread committed settings instead.
        const bool canRetryForSettings =
            command.Kind == AppCommandKind::ListDevices || command.Kind == AppCommandKind::Status ||
            command.Kind == AppCommandKind::ListAliases || command.Kind == AppCommandKind::ShowDefault;
        for (std::size_t attempt = 0; attempt != 3; ++attempt) {
            const auto settings = ReadCoherentSettings();
            if (!settings) {
                return MakeFailure(command.Kind, AppResultCode::InternalError, AppOutcomeReason::InternalError);
            }
            auto result = ExecuteCommand(command, context, settings->Data, settings->Revision);
            result.Command = command.Kind;
            result.DispatchPhase = AppDispatchPhase::Started;
            if (!canRetryForSettings || IsCurrentSettingsRevision(settings->Revision)) return result;
        }
        return MakeFailure(command.Kind, AppResultCode::InternalError, AppOutcomeReason::InternalError);
    } catch (...) {
        preflight.DispatchPhase = AppDispatchPhase::Started;
        preflight.Code = AppResultCode::InternalError;
        preflight.Reason = AppOutcomeReason::InternalError;
        return preflight;
    }
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
        for (std::size_t attempt = 0; attempt != 3; ++attempt) {
            const auto settings = ReadCoherentSettings();
            if (!settings) break;
            auto devices = BuildDevicesWithoutRefresh(settings->Data);
            auto snapshot = SnapshotFromDevices(std::move(devices), settings->Data, settings->Revision);
            if (snapshot.IsRunning || IsCurrentSettingsRevision(settings->Revision)) return snapshot;
        }
        AppSnapshot unavailable;
        unavailable.IsRunning = false;
        return unavailable;
    } catch (...) {
        AppSnapshot unavailable;
        unavailable.IsRunning = false;
        return unavailable;
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Use Cases /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::ExecuteCommand(AppCommand const& command,
                                        AppCommandContext const& context,
                                        SettingsData const& settings,
                                        std::uint64_t settingsRevision) const {
    const auto requiresRefresh = [&]() noexcept {
        if (command.Kind == AppCommandKind::ListDevices) return true;
        if (!command.Target) return false;
        return IsRefreshNeeded(command.Kind, command.Target->Kind());
    }();
    const auto requiresDevices = [&]() noexcept {
        switch (command.Kind) {
            case AppCommandKind::ListDevices:
            case AppCommandKind::Status:
            case AppCommandKind::ShowDefault:
            case AppCommandKind::ListAliases:
            case AppCommandKind::SetDefault:
            case AppCommandKind::SetAlias:
            case AppCommandKind::ClearAlias:
            case AppCommandKind::Connect:
            case AppCommandKind::Disconnect:
            case AppCommandKind::Reconnect:
            case AppCommandKind::ToggleLast:
            case AppCommandKind::ReconnectAll: return true;
            case AppCommandKind::ShowDevicePicker:
            case AppCommandKind::ShowSettings:
            case AppCommandKind::ClearDefault:
            case AppCommandKind::DisconnectAll: return false;
        }
        return false;
    }();
    std::vector<DeviceRecord> devices;
    if (requiresDevices) {
        devices = requiresRefresh ? BuildDevices(true, context, settings) : BuildDevicesWithoutRefresh(settings);
    }

    switch (command.Kind) {
        case AppCommandKind::ShowDevicePicker:
        case AppCommandKind::ShowSettings: {
            UiActionResult actionResult;
            if (command.Kind == AppCommandKind::ShowDevicePicker) {
                if (auto presentation = m_presentation.lock()) {
                    if (const auto code = MutationAdmissionFailure(context)) {
                        return MakeFailure(command.Kind, *code, AppOutcomeReason::NotReady, settings);
                    }
                    actionResult = presentation->PresentDevicePicker(command.PickerOpenMode, context);
                }
            } else if (auto presentation = m_presentation.lock()) {
                if (const auto code = MutationAdmissionFailure(context)) {
                    return MakeFailure(command.Kind, *code, AppOutcomeReason::NotReady, settings);
                }
                actionResult = presentation->PresentSettings(context);
            }
            if (actionResult.Status != OperationStatus::Succeeded) {
                const auto code = actionResult.Status == OperationStatus::Cancelled       ? AppResultCode::Cancelled
                                  : actionResult.Status == OperationStatus::TimedOut      ? AppResultCode::TimedOut
                                  : actionResult.Status == OperationStatus::Indeterminate ? AppResultCode::Indeterminate
                                                                                          : AppResultCode::Unavailable;
                return MakeFailure(command.Kind, code, AppOutcomeReason::NotReady, settings);
            }

            if (command.Kind == AppCommandKind::ShowDevicePicker) {
                std::uint64_t openedGeneration = 0;
                {
                    std::scoped_lock lock(m_stateMutex);
                    openedGeneration = m_pickerGeneration;
                }
                if (actionResult.DevicePickerOpenedGeneration) {
                    openedGeneration = *actionResult.DevicePickerOpenedGeneration;
                } else if (auto presentation = m_presentation.lock()) {
                    try {
                        openedGeneration = presentation->PickerOpenedGeneration();
                    } catch (...) {
                    }
                }
                std::scoped_lock lock(m_stateMutex);
                m_pickerGeneration = openedGeneration;
                AdvanceGeneration(m_generation);
            } else {
                std::scoped_lock lock(m_stateMutex);
                AdvanceGeneration(m_generation);
            }

            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.Reason = command.Kind == AppCommandKind::ShowDevicePicker ? AppOutcomeReason::ShowOpened
                                                                             : AppOutcomeReason::SettingsOpened;
            result.PrivacyModeEnabled = PrivacyMode(settings);
            return result;
        }
        case AppCommandKind::ListDevices:
        case AppCommandKind::Status:
        case AppCommandKind::ListAliases: {
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            for (auto const& device : devices) {
                if (auto snapshot = ToSnapshot(device)) result.Devices.push_back(std::move(*snapshot));
            }
            result.Snapshot = SnapshotFromDevices(devices, settings, settingsRevision);
            result.PrivacyModeEnabled = result.Snapshot->PrivacyModeEnabled;
            return result;
        }
        case AppCommandKind::ShowDefault: {
            auto snapshot = SnapshotFromDevices(devices, settings, settingsRevision);
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.DefaultDevice = snapshot.DefaultDevice;
            result.Snapshot = std::move(snapshot);
            result.PrivacyModeEnabled = result.Snapshot->PrivacyModeEnabled;
            return result;
        }
        case AppCommandKind::SetDefault: {
            auto resolution = Resolve(*command.Target, devices, settings);
            if (resolution.Code != AppResultCode::Success) {
                return MakeTargetResult(command.Kind, resolution, settings, resolution.Code, resolution.Reason);
            }
            if (!resolution.HasTarget || !resolution.Target.Exists) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::NotFound, AppOutcomeReason::TargetNotFound);
            }

            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
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
                        command.Kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::DefaultSet);
                }
                return MakeTargetResult(command.Kind,
                                        resolution,
                                        settings,
                                        AppResultCode::OperationFailed,
                                        AppOutcomeReason::InternalError);
            }
            const auto committedSettings = ReadCoherentSettings();
            if (!committedSettings) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::InternalError, AppOutcomeReason::InternalError);
            }
            auto committedSnapshot = SnapshotFromDevices(devices, committedSettings->Data, committedSettings->Revision);
            auto result = MakeTargetResult(command.Kind,
                                           resolution,
                                           committedSettings->Data,
                                           AppResultCode::Success,
                                           AppOutcomeReason::DefaultSet);
            result.DefaultDevice = committedSnapshot.DefaultDevice;
            return result;
        }
        case AppCommandKind::ClearDefault: {
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
            }
            const bool accepted = (m_settings->ClearDefaultDevice().Status != SettingsMutationStatus::Rejected);
            if (!accepted) {
                return MakeFailure(
                    command.Kind, AppResultCode::OperationFailed, AppOutcomeReason::InternalError, settings);
            }
            const auto committedSettings = ReadCoherentSettings();
            if (!committedSettings) {
                return MakeFailure(
                    command.Kind, AppResultCode::InternalError, AppOutcomeReason::InternalError, settings);
            }
            auto committedSnapshot = SnapshotFromDevices({}, committedSettings->Data, committedSettings->Revision);
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.Reason = AppOutcomeReason::DefaultCleared;
            result.DefaultDevice = committedSnapshot.DefaultDevice;
            result.PrivacyModeEnabled = PrivacyMode(committedSettings->Data);
            return result;
        }
        case AppCommandKind::SetAlias:
        case AppCommandKind::ClearAlias: {
            auto resolution = Resolve(*command.Target, devices, settings);
            if (resolution.Code != AppResultCode::Success) {
                return MakeTargetResult(command.Kind, resolution, settings, resolution.Code, resolution.Reason);
            }
            if (!resolution.HasTarget || !resolution.Target.Exists) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::NotFound, AppOutcomeReason::TargetNotFound);
            }

            const auto alias = command.Kind == AppCommandKind::SetAlias ? command.Alias : std::wstring{};
            if (!apc::limits::IsBoundedUtf16(alias, apc::limits::c_maxDeviceAliasCharacters)) {
                return MakeTargetResult(command.Kind,
                                        resolution,
                                        settings,
                                        AppResultCode::OperationFailed,
                                        command.Kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSetFailed
                                                                                 : AppOutcomeReason::AliasClearFailed);
            }
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
            }
            const bool accepted =
                (m_settings->SetDeviceAlias(resolution.Target.Id, alias, resolution.Target.Name).Mutation.Status !=
                 SettingsMutationStatus::Rejected);
            if (!accepted) {
                return MakeTargetResult(command.Kind,
                                        resolution,
                                        settings,
                                        AppResultCode::OperationFailed,
                                        command.Kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSetFailed
                                                                                 : AppOutcomeReason::AliasClearFailed);
            }

            const auto committedSettings = ReadCoherentSettings();
            if (!committedSettings) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::InternalError, AppOutcomeReason::InternalError);
            }
            auto refreshedDevices = BuildDevicesWithoutRefresh(committedSettings->Data);
            auto result = MakeTargetResult(command.Kind,
                                           resolution,
                                           committedSettings->Data,
                                           AppResultCode::Success,
                                           command.Kind == AppCommandKind::SetAlias ? AppOutcomeReason::AliasSet
                                                                                    : AppOutcomeReason::AliasCleared);
            const auto committedDevice =
                std::ranges::find_if(committedSettings->Data.Devices, [&resolution](auto const& device) {
                    return EqualsIgnoreCase(device.Id, resolution.Target.Id);
                });
            result.Alias =
                committedDevice == committedSettings->Data.Devices.end() ? std::wstring{} : committedDevice->Alias;
            result.Device = PostOperationDevice(resolution.Target.Id, refreshedDevices);
            return result;
        }
        case AppCommandKind::Connect:
        case AppCommandKind::Disconnect:
        case AppCommandKind::Reconnect: return ExecuteTargetOperation(command, context, devices, settings);
        case AppCommandKind::ToggleLast: return ExecuteToggle(command, context, devices, settings);
        case AppCommandKind::DisconnectAll: {
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
            }
            (void)m_devices->DisconnectAll();
            {
                std::scoped_lock lock(m_stateMutex);
                AdvanceGeneration(m_generation);
            }
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.Reason = AppOutcomeReason::DisconnectAllSucceeded;
            result.PrivacyModeEnabled = PrivacyMode(settings);
            return result;
        }
        case AppCommandKind::ReconnectAll: {
            if (context.Completion == AppCommandContext::CompletionMode::Detached) {
                if (const auto code = MutationAdmissionFailure(context)) {
                    return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
                }
                (void)m_devices->ReconnectAll();
                {
                    std::scoped_lock lock(m_stateMutex);
                    AdvanceGeneration(m_generation);
                }
                AppResult result;
                result.Code = AppResultCode::Success;
                result.Command = command.Kind;
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
                    return MakeTargetResult(command.Kind,
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
                    const auto reason = operation.Status == OperationStatus::Cancelled
                                            ? AppOutcomeReason::NotReady
                                            : AppOutcomeReason::ReconnectFailed;
                    return MakeTargetResult(command.Kind, resolution, settings, code, reason);
                }
                auto after = BuildDevicesWithoutRefresh(settings);
                auto current = FindById(after, device.Id);
                if (!current || !current->IsConnected) {
                    return MakeTargetResult(command.Kind,
                                            resolution,
                                            settings,
                                            AppResultCode::OperationFailed,
                                            AppOutcomeReason::ReconnectFailed);
                }
            }
            {
                std::scoped_lock lock(m_stateMutex);
                AdvanceGeneration(m_generation);
            }
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.Reason = AppOutcomeReason::ReconnectAllSucceeded;
            result.PrivacyModeEnabled = PrivacyMode(settings);
            return result;
        }
    }

    return MakeFailure(command.Kind, AppResultCode::InvalidInput, AppOutcomeReason::Unsupported, settings);
}

AppResult AppController::ExecuteTargetOperation(AppCommand const& command,
                                                AppCommandContext const& context,
                                                std::vector<DeviceRecord> const& devices,
                                                SettingsData const& settings) const {
    auto resolution = Resolve(*command.Target, devices, settings);
    if (resolution.Code != AppResultCode::Success) {
        return MakeTargetResult(command.Kind, resolution, settings, resolution.Code, resolution.Reason);
    }

    const auto id = resolution.Target.Id;
    if (command.Kind == AppCommandKind::Connect && resolution.Target.IsConnected) {
        return MakeTargetResult(
            command.Kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::AlreadyConnected);
    }
    if (command.Kind == AppCommandKind::Disconnect && !resolution.Target.IsConnected) {
        return MakeTargetResult(
            command.Kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::AlreadyDisconnected);
    }

    const bool detached = context.Completion == AppCommandContext::CompletionMode::Detached;
    if (command.Kind == AppCommandKind::Disconnect) {
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        (void)m_devices->Disconnect(id);
        auto after = BuildDevicesWithoutRefresh(settings);
        auto current = FindById(after, id);
        if (current && current->IsConnected) {
            return MakeTargetResult(
                command.Kind, resolution, settings, AppResultCode::OperationFailed, AppOutcomeReason::DisconnectFailed);
        }
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        auto result = MakeTargetResult(
            command.Kind, resolution, settings, AppResultCode::Success, AppOutcomeReason::DisconnectSucceeded);
        result.Device = PostOperationDevice(id, after);
        return result;
    }

    if (detached) {
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        auto const admitted =
            command.Kind == AppCommandKind::Connect ? m_devices->Connect(id) : m_devices->Reconnect(id);
        if (admitted.Kind == apc::device::DeviceCommandResultKind::Rejected)
            return MakeTargetResult(command.Kind, resolution, settings, AppResultCode::Busy, AppOutcomeReason::None);
        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        return MakeTargetResult(command.Kind,
                                resolution,
                                settings,
                                AppResultCode::Success,
                                command.Kind == AppCommandKind::Connect ? AppOutcomeReason::ConnectSucceeded
                                                                        : AppOutcomeReason::ReconnectSucceeded);
    }

    if (const auto code = MutationAdmissionFailure(context)) {
        return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
    }
    const auto operationResult = PerformDeviceOperation(command.Kind, id, context);
    if (!IsSuccess(operationResult.Status)) {
        const auto code = operationResult.Status == OperationStatus::Failed ? AppResultCode::Indeterminate
                                                                            : ToResultCode(operationResult.Status);
        return MakeTargetResult(command.Kind, resolution, settings, code, AppOutcomeReason::None);
    }

    auto after = BuildDevicesWithoutRefresh(settings);
    auto current = FindById(after, id);
    if (!current || !current->IsConnected) {
        return MakeTargetResult(
            command.Kind, resolution, settings, AppResultCode::OperationFailed, OperationReason(command.Kind));
    }

    {
        std::scoped_lock lock(m_stateMutex);
        AdvanceGeneration(m_generation);
    }
    auto result = MakeTargetResult(command.Kind,
                                   resolution,
                                   settings,
                                   AppResultCode::Success,
                                   command.Kind == AppCommandKind::Connect ? AppOutcomeReason::ConnectSucceeded
                                                                           : AppOutcomeReason::ReconnectSucceeded);
    result.Device = PostOperationDevice(id, after);
    return result;
}

AppResult AppController::ExecuteToggle(AppCommand const& command,
                                       AppCommandContext const& context,
                                       std::vector<DeviceRecord> const& devices,
                                       SettingsData const& settings) const {
    auto resolution = Resolve(*command.Target, devices, settings);
    if (resolution.Code != AppResultCode::Success) {
        return MakeTargetResult(command.Kind, resolution, settings, resolution.Code, resolution.Reason);
    }
    if (context.Completion == AppCommandContext::CompletionMode::Detached) {
        const bool globalBusy = m_devices->HasBusyOperations();
        const bool deviceBusy = m_devices->IsDeviceBusy(resolution.Target.Id);
        if (globalBusy || deviceBusy) {
            return MakeTargetResult(command.Kind, resolution, settings, AppResultCode::Busy, AppOutcomeReason::None);
        }
    }

    AppCommand operationCommand{
        resolution.Target.IsConnected ? AppCommandKind::Disconnect : AppCommandKind::Connect, command.Target, {}};
    auto result = ExecuteTargetOperation(operationCommand, context, devices, settings);
    result.Command = command.Kind;
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
    std::vector<DeviceRecord> refreshed;
    if (refresh) {
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
            if (completed && operation.Status() == winrt::Windows::Foundation::AsyncStatus::Completed) {
                if (auto inventory = operation.GetResults()) {
                    for (auto const& device : inventory)
                        refreshed.push_back({std::wstring(device.Id()),
                                             std::wstring(device.Name()),
                                             {},
                                             DeviceConnectionState::Idle,
                                             false,
                                             true,
                                             false});
                }
            }
        } catch (winrt::hresult_error const&) {
            // Discovery is optional: retain known settings and live sessions when
            // Windows cannot enumerate devices. Mutation admission still checks
            // the caller's cancellation and deadline after this fallback.
            refreshed.clear();
        }
    }
    return MergeDevices(std::move(refreshed), ReadConnectedDevices(), settings);
}

std::vector<AppController::DeviceRecord> AppController::BuildDevicesWithoutRefresh(SettingsData const& settings) const {
    return MergeDevices({}, ReadConnectedDevices(), settings);
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

bool AppController::IsCurrentSettingsRevision(std::uint64_t revision) const noexcept {
    std::scoped_lock lock(m_stateMutex);
    return m_lastSettingsRevision && revision == *m_lastSettingsRevision;
}

std::vector<AppController::DeviceRecord> AppController::ReadConnectedDevices() const {
    std::vector<DeviceRecord> records;
    auto const snapshot = m_devices->Snapshot();
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

AppSnapshot AppController::SnapshotFromDevices(std::vector<DeviceRecord> devices,
                                               SettingsData const& settings,
                                               std::uint64_t settingsRevision) const noexcept {
    try {
        std::uint64_t generation = 0;
        std::uint64_t pickerGeneration = 0;
        bool isRunning = false;
        {
            std::scoped_lock lock(m_stateMutex);
            generation = m_generation;
            pickerGeneration = m_pickerGeneration;
            isRunning = m_running;
        }
        if (!isRunning) {
            AppSnapshot unavailable;
            unavailable.Generation = generation;
            unavailable.IsRunning = false;
            return unavailable;
        }

        if (auto presentation = m_presentation.lock()) {
            try {
                pickerGeneration = std::max(pickerGeneration, presentation->PickerOpenedGeneration());
            } catch (...) {
            }
        }

        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_running) isRunning = false;
            // This is the snapshot's linearization point. A SettingsData
            // value is never stamped with a generation that was advanced for
            // a newer Store revision by another caller.
            if (!m_lastSettingsRevision || *m_lastSettingsRevision != settingsRevision) {
                AppSnapshot unavailable;
                unavailable.Generation = m_generation;
                unavailable.IsRunning = false;
                return unavailable;
            }
            generation = m_generation;
        }
        return BuildSnapshot(std::move(devices), settings, generation, pickerGeneration, isRunning);
    } catch (...) {
        AppSnapshot unavailable;
        unavailable.IsRunning = false;
        return unavailable;
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Result Projection /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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

bool AppController::IsRefreshNeeded(AppCommandKind command, DeviceSelectorKind selectorKind) noexcept {
    if (command == AppCommandKind::ListDevices) return true;
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

} // namespace apc::app
