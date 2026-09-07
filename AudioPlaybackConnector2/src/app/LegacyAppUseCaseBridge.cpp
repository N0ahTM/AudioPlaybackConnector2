#include <app/LegacyAppUseCaseBridge.hpp>

#include <core/SettingsLimits.hpp>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace apc::app {

namespace {

using DeviceRecord = LegacyAppUseCaseBridge::DeviceRecord;
using OperationStatus = LegacyAppUseCaseBridge::OperationStatus;

struct BridgeReadFailure final {};

std::wstring LowerInvariant(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (const auto character : value) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return lowered;
}

std::wstring DeviceName(DeviceRecord const& device) {
    if (!device.Alias.empty()) return device.Alias;
    return device.Name.empty() ? device.Id : device.Name;
}

bool IsTerminal(OperationStatus status) noexcept {
    return status == OperationStatus::Succeeded || status == OperationStatus::Failed ||
           status == OperationStatus::Cancelled || status == OperationStatus::TimedOut ||
           status == OperationStatus::Indeterminate;
}

bool IsBusyState(DeviceConnectionState state) noexcept {
    return state == DeviceConnectionState::Connecting || state == DeviceConnectionState::Disconnecting ||
           state == DeviceConnectionState::WaitingForReconnect;
}

} // namespace

LegacyAppUseCaseBridge::LegacyAppUseCaseBridge(Operations operations) : m_operations(std::move(operations)) {}

LegacyAppUseCaseBridge::CallLease::CallLease(LegacyAppUseCaseBridge const& owner) noexcept : m_owner(owner) {
    std::scoped_lock lock(m_owner.m_stateMutex);
    if (!m_owner.m_running) return;
    ++m_owner.m_activeCalls;
    m_acquired = true;
}

LegacyAppUseCaseBridge::CallLease::~CallLease() {
    if (!m_acquired) return;
    std::scoped_lock lock(m_owner.m_stateMutex);
    --m_owner.m_activeCalls;
    if (m_owner.m_activeCalls == 0) m_owner.m_noActiveCalls.notify_all();
}

AppResult LegacyAppUseCaseBridge::Execute(AppCommand const& command, AppCommandContext context) noexcept {
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
            if (!canRetryForSettings || IsCurrentSettingsRevision(settings->Revision)) return result;
        }
        return MakeFailure(command.Kind, AppResultCode::InternalError, AppOutcomeReason::InternalError);
    } catch (...) {
        preflight.Code = AppResultCode::InternalError;
        preflight.Reason = AppOutcomeReason::InternalError;
        return preflight;
    }
}

AppSnapshot LegacyAppUseCaseBridge::Snapshot() const noexcept {
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

std::optional<AppEvent> LegacyAppUseCaseBridge::Observe(DeviceFact fact) noexcept {
    try {
        CallLease lease(*this);
        if (!lease.Acquired()) return std::nullopt;
        const bool requiresId =
            fact.Kind != FactKind::DeviceActivityChanged && fact.Kind != FactKind::DeviceInventoryChanged;
        std::optional<ExternalDeviceId> id;
        if (requiresId) {
            id = ExternalDeviceId::TryCreate(fact.Id);
            if (!id) return std::nullopt;
        }

        if (id) {
            std::scoped_lock lock(m_stateMutex);
            if (fact.Kind == FactKind::DeviceDisconnected) {
                m_observedStates.erase(fact.Id);
            } else if (fact.Kind == FactKind::DeviceConnected) {
                m_observedStates[fact.Id] =
                    DeviceRecord{fact.Id, {}, {}, DeviceConnectionState::Connected, true, true, false};
            } else if (fact.Kind == FactKind::DeviceStatusChanged) {
                m_observedStates[fact.Id] = DeviceRecord{fact.Id,
                                                         {},
                                                         {},
                                                         fact.State,
                                                         fact.State == DeviceConnectionState::Connected,
                                                         true,
                                                         IsBusyState(fact.State)};
            } else if (fact.Kind == FactKind::ConnectionError || fact.Kind == FactKind::AutoReconnectFailed) {
                m_observedStates[fact.Id] =
                    DeviceRecord{fact.Id, {}, {}, DeviceConnectionState::Failed, false, true, false};
            } else if (fact.Kind == FactKind::AutoReconnectTriggered) {
                m_observedStates[fact.Id] =
                    DeviceRecord{fact.Id, {}, {}, DeviceConnectionState::WaitingForReconnect, false, true, true};
            }
        }

        std::optional<AppEvent> event;
        switch (fact.Kind) {
            case FactKind::DeviceConnected: event = DeviceConnectedEvent{*id}; break;
            case FactKind::DeviceDisconnected: event = DeviceDisconnectedEvent{*id}; break;
            case FactKind::ConnectionError:
                event = DeviceConnectionErrorEvent{
                    *id, fact.ErrorCode == AppResultCode::Success ? AppResultCode::OperationFailed : fact.ErrorCode};
                break;
            case FactKind::DeviceStatusChanged: event = DeviceStatusChangedEvent{*id, fact.State}; break;
            case FactKind::DeviceActivityChanged: event = DeviceActivityChangedEvent{}; break;
            case FactKind::DeviceInventoryChanged: event = DeviceInventoryChangedEvent{}; break;
            case FactKind::AutoReconnectTriggered: event = AutoReconnectTriggeredEvent{*id}; break;
            case FactKind::AutoReconnectFailed: event = AutoReconnectFailedEvent{*id}; break;
        }

        {
            std::scoped_lock lock(m_stateMutex);
            AdvanceGeneration(m_generation);
        }
        return event;
    } catch (...) {
        return std::nullopt;
    }
}

void LegacyAppUseCaseBridge::SetRunning(bool running) noexcept {
    try {
        std::unique_lock lock(m_stateMutex);
        // Shutdown is monotonic.  A late composition callback must not make a
        // stopped bridge advertise a live app or accept another command.
        if (!m_running) return;
        if (running) return;
        m_running = running;
        AdvanceGeneration(m_generation);
        // Do not retain the state lock while waiting: admitted commands may
        // need it to finish their final state update after an external call.
        m_noActiveCalls.wait(lock, [this] { return m_activeCalls == 0; });
    } catch (...) {
    }
}

AppResult LegacyAppUseCaseBridge::ExecuteCommand(AppCommand const& command,
                                                 AppCommandContext const& context,
                                                 SettingsData const& settings,
                                                 std::uint64_t settingsRevision) {
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
                if (m_operations.ShowDevicePicker) {
                    if (const auto code = MutationAdmissionFailure(context)) {
                        return MakeFailure(command.Kind, *code, AppOutcomeReason::NotReady, settings);
                    }
                    actionResult = m_operations.ShowDevicePicker(command.PickerOpenMode, context);
                }
            } else if (m_operations.ShowSettings) {
                if (const auto code = MutationAdmissionFailure(context)) {
                    return MakeFailure(command.Kind, *code, AppOutcomeReason::NotReady, settings);
                }
                actionResult = m_operations.ShowSettings(context);
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
                } else if (m_operations.PickerOpenedGeneration) {
                    try {
                        openedGeneration = m_operations.PickerOpenedGeneration();
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

            if (!m_operations.SetDefaultDevice) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
            }
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
            }
            const bool accepted = m_operations.SetDefaultDevice(resolution.Target.Id);
            if (!accepted) {
                // A P01 external device ID may be longer than the bounded
                // P07 persistence identity. The Store correctly rejects that
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
            if (!m_operations.ClearDefaultDevice) {
                return MakeFailure(command.Kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady, settings);
            }
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
            }
            const bool accepted = m_operations.ClearDefaultDevice();
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
            if (!m_operations.SetDeviceAlias) {
                return MakeTargetResult(
                    command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
            }
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
            }
            const bool accepted = m_operations.SetDeviceAlias(resolution.Target.Id, alias, resolution.Target.Name);
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
            if (!m_operations.DisconnectAll) {
                return MakeFailure(command.Kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady, settings);
            }
            if (const auto code = MutationAdmissionFailure(context)) {
                return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
            }
            m_operations.DisconnectAll();
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
                if (!m_operations.ReconnectAllDetached) {
                    return MakeFailure(command.Kind, AppResultCode::Unavailable, AppOutcomeReason::NotReady, settings);
                }
                if (const auto code = MutationAdmissionFailure(context)) {
                    return MakeFailure(command.Kind, *code, AppOutcomeReason::None, settings);
                }
                m_operations.ReconnectAllDetached();
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
                if (!m_operations.Reconnect) {
                    return MakeTargetResult(
                        command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
                }
                const auto operation = m_operations.Reconnect(device.Id, context);
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

AppResult LegacyAppUseCaseBridge::ExecuteTargetOperation(AppCommand const& command,
                                                         AppCommandContext const& context,
                                                         std::vector<DeviceRecord> const& devices,
                                                         SettingsData const& settings) {
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
        if (!m_operations.Disconnect) {
            return MakeTargetResult(
                command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
        }
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        m_operations.Disconnect(id);
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
        const auto operation =
            command.Kind == AppCommandKind::Connect ? m_operations.ConnectDetached : m_operations.ReconnectDetached;
        if (!operation) {
            return MakeTargetResult(
                command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
        }
        if (const auto code = MutationAdmissionFailure(context)) {
            return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
        }
        operation(id);
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

    const auto operation = command.Kind == AppCommandKind::Connect ? m_operations.Connect : m_operations.Reconnect;
    if (!operation) {
        return MakeTargetResult(
            command.Kind, resolution, settings, AppResultCode::Unavailable, AppOutcomeReason::NotReady);
    }
    if (const auto code = MutationAdmissionFailure(context)) {
        return MakeTargetResult(command.Kind, resolution, settings, *code, AppOutcomeReason::None);
    }
    const auto operationResult = operation(id, context);
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

AppResult LegacyAppUseCaseBridge::ExecuteToggle(AppCommand const& command,
                                                AppCommandContext const& context,
                                                std::vector<DeviceRecord> const& devices,
                                                SettingsData const& settings) {
    auto resolution = Resolve(*command.Target, devices, settings);
    if (resolution.Code != AppResultCode::Success) {
        return MakeTargetResult(command.Kind, resolution, settings, resolution.Code, resolution.Reason);
    }
    if (context.Completion == AppCommandContext::CompletionMode::Detached) {
        bool globalBusy = false;
        if (m_operations.HasBusy) {
            globalBusy = m_operations.HasBusy();
        } else {
            globalBusy = std::ranges::any_of(devices, [](auto const& device) { return device.IsBusy; });
        }
        const bool deviceBusy = m_operations.DeviceBusy ? m_operations.DeviceBusy(resolution.Target.Id)
                                                        : (resolution.Device && resolution.Device->IsBusy);
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

LegacyAppUseCaseBridge::Resolution LegacyAppUseCaseBridge::Resolve(DeviceSelector const& selector,
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

std::vector<LegacyAppUseCaseBridge::DeviceRecord>
LegacyAppUseCaseBridge::BuildDevices(bool refresh, AppCommandContext const& context, SettingsData const& settings) {
    auto connected = ReadConnectedDevices();
    std::vector<DeviceRecord> refreshed;
    if (refresh && m_operations.Refresh) {
        auto refreshContext = CappedRefreshContext(context);
        RefreshResult refreshResult;
        try {
            refreshResult = m_operations.Refresh(refreshContext);
        } catch (...) {
            refreshResult.Status = OperationStatus::Failed;
        }
        const bool completed = refreshResult.Status == OperationStatus::Succeeded && IsTerminal(refreshResult.Status) &&
                               !refreshContext.IsCancellationRequested() &&
                               !refreshContext.IsExpired(AppCommandContext::Clock::now());
        if (completed) {
            refreshed = refreshResult.Devices;
        }
    }
    return MergeDevices(std::move(refreshed), std::move(connected), settings);
}

std::vector<LegacyAppUseCaseBridge::DeviceRecord>
LegacyAppUseCaseBridge::BuildDevicesWithoutRefresh(SettingsData const& settings) const {
    return MergeDevices({}, ReadConnectedDevices(), settings);
}

std::optional<SettingsSnapshot> LegacyAppUseCaseBridge::ReadSettings() const noexcept {
    if (!m_operations.ReadSettings) return std::nullopt;
    try {
        auto snapshot = m_operations.ReadSettings();
        // Only the Store revision crosses into bridge state. SettingsData
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

std::optional<SettingsSnapshot> LegacyAppUseCaseBridge::ReadCoherentSettings() const noexcept {
    // SettingsStore snapshots are monotonic. A stale callback completion can
    // only be reconciled by taking another value snapshot; never cache its
    // SettingsData in the bridge or hold bridge state while invoking Store.
    for (std::size_t attempt = 0; attempt != 3; ++attempt) {
        const auto snapshot = ReadSettings();
        if (!snapshot) return std::nullopt;

        std::scoped_lock lock(m_stateMutex);
        if (m_lastSettingsRevision && snapshot->Revision == *m_lastSettingsRevision) return snapshot;
    }
    return std::nullopt;
}

bool LegacyAppUseCaseBridge::IsCurrentSettingsRevision(std::uint64_t revision) const noexcept {
    std::scoped_lock lock(m_stateMutex);
    return m_lastSettingsRevision && revision == *m_lastSettingsRevision;
}

std::vector<LegacyAppUseCaseBridge::DeviceRecord> LegacyAppUseCaseBridge::ReadConnectedDevices() const {
    if (!m_operations.ReadConnectedDevices) return {};
    try {
        return m_operations.ReadConnectedDevices();
    } catch (...) {
        throw BridgeReadFailure{};
    }
}

std::vector<LegacyAppUseCaseBridge::DeviceRecord> LegacyAppUseCaseBridge::MergeDevices(
    std::vector<DeviceRecord> refreshed, std::vector<DeviceRecord> connected, SettingsData const& settings) const {
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
    // ApplyObservedStates needs the unmodified session records below: they
    // remain the authority for connection truth after the presentation merge.
    for (auto const& device : connected)
        upsert(device);
    for (auto const& device : settings.Devices) {
        upsert(DeviceRecord{device.Id, device.Name, device.Alias, DeviceConnectionState::Idle, false, true, false});
    }

    ApplyObservedStates(merged, connected);
    std::ranges::sort(merged, [](auto const& left, auto const& right) {
        const auto leftLabel = LowerInvariant(DeviceLabel(left));
        const auto rightLabel = LowerInvariant(DeviceLabel(right));
        if (leftLabel != rightLabel) return leftLabel < rightLabel;
        return LowerInvariant(left.Id) < LowerInvariant(right.Id);
    });
    return merged;
}

AppSnapshot LegacyAppUseCaseBridge::BuildSnapshot(std::vector<DeviceRecord> devices,
                                                  SettingsData const& settings,
                                                  std::uint64_t generation,
                                                  std::uint64_t pickerGeneration,
                                                  bool isRunning) const noexcept {
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
    if (m_operations.HasBusy) {
        try {
            snapshot.Tray.HasBusyOperations = m_operations.HasBusy();
        } catch (...) {
        }
    }
    for (auto const& device : snapshot.Devices) {
        if (device.IsConnected) snapshot.Tray.ConnectedDevices.push_back(device);
    }
    if (m_operations.ResourceStatus) {
        try {
            snapshot.AdaptiveResources = m_operations.ResourceStatus();
        } catch (...) {
        }
    }
    return snapshot;
}

AppSnapshot LegacyAppUseCaseBridge::SnapshotFromDevices(std::vector<DeviceRecord> devices,
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

        if (m_operations.Running) {
            try {
                isRunning = m_operations.Running();
            } catch (...) {
                isRunning = false;
            }
        }
        if (m_operations.PickerOpenedGeneration) {
            try {
                pickerGeneration = std::max(pickerGeneration, m_operations.PickerOpenedGeneration());
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

AppResult LegacyAppUseCaseBridge::MakeFailure(AppCommandKind command,
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

AppResult LegacyAppUseCaseBridge::MakeFailure(AppCommandKind command,
                                              AppResultCode code,
                                              AppOutcomeReason reason,
                                              SettingsData const& settings,
                                              std::wstring requestedTarget) const {
    auto result = MakeFailure(command, code, reason, std::move(requestedTarget));
    result.PrivacyModeEnabled = PrivacyMode(settings);
    return result;
}

AppResult LegacyAppUseCaseBridge::MakeTargetResult(AppCommandKind command,
                                                   Resolution const& resolution,
                                                   SettingsData const& settings,
                                                   AppResultCode code,
                                                   AppOutcomeReason reason) const {
    auto result = MakeFailure(command, code, reason, settings, resolution.RequestedTarget);
    if (resolution.HasTarget) result.Target = resolution.Target;
    if (resolution.Device) result.Device = ToSnapshot(*resolution.Device);
    return result;
}

std::optional<DeviceSnapshot> LegacyAppUseCaseBridge::ToSnapshot(DeviceRecord const& record) const {
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

std::optional<AppTargetSnapshot> LegacyAppUseCaseBridge::ToTarget(DeviceRecord const& record) const {
    return AppTargetSnapshot{
        record.Id, record.Name, record.Alias, DeviceLabel(record), true, record.IsConnected, record.IsKnown};
}

std::optional<DeviceSnapshot>
LegacyAppUseCaseBridge::PostOperationDevice(std::wstring_view deviceId,
                                            std::vector<DeviceRecord> const& devices) const {
    if (auto record = FindById(devices, deviceId)) return ToSnapshot(*record);
    return std::nullopt;
}

std::wstring LegacyAppUseCaseBridge::DeviceLabel(DeviceRecord const& device) {
    return DeviceName(device);
}

bool LegacyAppUseCaseBridge::EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return LowerInvariant(left) == LowerInvariant(right);
}

bool LegacyAppUseCaseBridge::ContainsIgnoreCase(std::wstring_view value, std::wstring_view query) {
    return !query.empty() && LowerInvariant(value).find(LowerInvariant(query)) != std::wstring::npos;
}

std::wstring LegacyAppUseCaseBridge::NormalizeHex(std::wstring_view value) {
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

std::optional<LegacyAppUseCaseBridge::DeviceRecord>
LegacyAppUseCaseBridge::FindById(std::vector<DeviceRecord> const& devices, std::wstring_view id) {
    auto found = std::ranges::find_if(devices, [id](auto const& device) { return EqualsIgnoreCase(device.Id, id); });
    if (found == devices.end()) return std::nullopt;
    return *found;
}

std::optional<apc::core::DeviceId> LegacyAppUseCaseBridge::TryDeviceId(std::wstring_view id) {
    return apc::core::DeviceId::TryCreate(id);
}

AppResultCode LegacyAppUseCaseBridge::ToResultCode(OperationStatus status) noexcept {
    switch (status) {
        case OperationStatus::Succeeded: return AppResultCode::Success;
        case OperationStatus::Failed: return AppResultCode::OperationFailed;
        case OperationStatus::Cancelled: return AppResultCode::Cancelled;
        case OperationStatus::TimedOut: return AppResultCode::TimedOut;
        case OperationStatus::Indeterminate: return AppResultCode::Indeterminate;
    }
    return AppResultCode::InternalError;
}

AppOutcomeReason LegacyAppUseCaseBridge::OperationReason(AppCommandKind command) noexcept {
    switch (command) {
        case AppCommandKind::Connect: return AppOutcomeReason::ConnectFailed;
        case AppCommandKind::Disconnect: return AppOutcomeReason::DisconnectFailed;
        case AppCommandKind::Reconnect:
        case AppCommandKind::ReconnectAll: return AppOutcomeReason::ReconnectFailed;
        default: return AppOutcomeReason::InternalError;
    }
}

bool LegacyAppUseCaseBridge::IsSuccess(OperationStatus status) noexcept {
    return status == OperationStatus::Succeeded;
}

std::optional<AppResultCode>
LegacyAppUseCaseBridge::MutationAdmissionFailure(AppCommandContext const& context) noexcept {
    if (context.IsCancellationRequested()) return AppResultCode::Cancelled;
    if (context.IsExpired(AppCommandContext::Clock::now())) return AppResultCode::TimedOut;
    return std::nullopt;
}

void LegacyAppUseCaseBridge::ApplyObservedStates(std::vector<DeviceRecord>& devices,
                                                 std::vector<DeviceRecord> const& connectedDevices) const {
    std::unordered_map<std::wstring, DeviceRecord> observed;
    {
        std::scoped_lock lock(m_stateMutex);
        observed = m_observedStates;
    }

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
        if (current.State == DeviceConnectionState::Connected) {
            current.State = DeviceConnectionState::Idle;
        }

        auto observedState = std::ranges::find_if(
            observed, [&current](auto const& entry) { return EqualsIgnoreCase(entry.first, current.Id); });
        if (observedState == observed.end()) continue;

        auto const& state = observedState->second;
        if (current.Name.empty()) current.Name = state.Name;
        if (current.Alias.empty()) current.Alias = state.Alias;
        current.IsKnown = current.IsKnown || state.IsKnown;
        if (state.IsConnected) continue;

        current.State = state.State;
        if (IsBusyState(state.State)) current.IsBusy = true;
    }
}

bool LegacyAppUseCaseBridge::IsRefreshNeeded(AppCommandKind command, DeviceSelectorKind selectorKind) noexcept {
    if (command == AppCommandKind::ListDevices) return true;
    return selectorKind == DeviceSelectorKind::Name || selectorKind == DeviceSelectorKind::Mac ||
           selectorKind == DeviceSelectorKind::Auto;
}

AppCommandContext LegacyAppUseCaseBridge::CappedRefreshContext(AppCommandContext const& context) {
    auto capped = context;
    const auto now = AppCommandContext::Clock::now();
    const auto localDeadline = now + c_refreshTimeout;
    if (capped.Deadline == AppCommandContext::TimePoint::max() || capped.Deadline > localDeadline) {
        capped.Deadline = localDeadline;
    }
    return capped;
}

void LegacyAppUseCaseBridge::AdvanceGeneration(std::uint64_t& generation) noexcept {
    if (generation != std::numeric_limits<std::uint64_t>::max()) ++generation;
}

bool LegacyAppUseCaseBridge::PrivacyMode(SettingsData const& settings) const noexcept {
    return settings.PrivacyModeEnabled;
}

} // namespace apc::app
