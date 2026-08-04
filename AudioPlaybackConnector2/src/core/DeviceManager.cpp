#include <pch.h>
#include <core/DeviceManager.hpp>
#include <core/AudioConnectionService.hpp>
#include <core/DeviceManagerDiagnostics.hpp>
#include <core/StringResources.hpp>
#include <thread>
#include <utility>

namespace {
constexpr int c_heartbeatIntervalMinutes = 5;
constexpr std::chrono::seconds c_userActionCascadeWindow{5};
constexpr std::size_t c_openTransientFailureMaxAttempts = 10;

std::chrono::milliseconds OpenTransientRetryDelay(std::size_t failedAttempt) noexcept {
    constexpr std::array<int, c_openTransientFailureMaxAttempts - 1> delaysMs{
        500, 1000, 1500, 2500, 4000, 6000, 8000, 8000, 8000};
    if (failedAttempt == 0) return std::chrono::milliseconds(0);
    const auto index = std::min(failedAttempt - 1, delaysMs.size() - 1);
    return std::chrono::milliseconds(delaysMs[index]);
}

bool IsRetryableOpenFailure(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus status,
                            winrt::hresult extendedError) noexcept {
    using winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
    if (status == AudioPlaybackConnectionOpenResultStatus::RequestTimedOut) return true;
    if (status != AudioPlaybackConnectionOpenResultStatus::UnknownFailure) return false;
    return static_cast<HRESULT>(extendedError) == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
}

inline void ReportAsyncConnectionError(DeviceManager& dm,
                                       winrt::hstring const& deviceId,
                                       winrt::hstring const& message,
                                       std::wstring_view context) {
    DebugTrace(L"[DeviceManager] {0} ERROR: {1}", std::wstring(context), std::wstring(message));
    dm.ConnectionError(deviceId, message);
    dm.DeviceStatusChanged(deviceId,
                           message,
                           winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                           DeviceStatusKind::Error);
    dm.DeviceActivityChanged(deviceId);
}

void PopulateConnectionMetadata(DeviceConnectionInfo& info,
                                winrt::Windows::Devices::Enumeration::DeviceInformation const& device) {
    try {
        info.Id = std::wstring(device.Id());
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {
    }
    try {
        info.Name = std::wstring(device.Name());
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {
    }
    if (info.Id.empty()) {
        info.Id = info.Name;
    }
    if (info.Name.empty()) {
        info.Name = info.Id;
    }
}

void CloseConnectionsOnBackgroundThread(std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> connections,
                                        std::wstring_view context,
                                        std::function<void()> completed = {}) noexcept {
    if (connections.empty()) return;

    auto sharedConnections =
        std::make_shared<std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>>(std::move(connections));
    auto closeConnections = [completed = std::move(completed)](
                                std::shared_ptr<std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>>
                                    connectionsToClose) noexcept {
        for (auto& connection : *connectionsToClose) {
            AudioConnectionService::Close(connection);
        }
        if (completed) {
            try {
                completed();
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] Connection cleanup completion ignored exception");
            }
        }
    };

    try {
        (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
            [sharedConnections, closeConnections](winrt::Windows::Foundation::IAsyncAction) noexcept {
                closeConnections(sharedConnections);
            });
        return;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(std::format(L"[DeviceManager] {0} scheduling failed", context), ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(std::format(L"[DeviceManager] {0} scheduling failed", context), ex);
    } catch (...) {
        util::DebugTraceUnknownException(std::format(L"[DeviceManager] {0} scheduling failed", context));
    }

    try {
        std::thread([sharedConnections, closeConnections]() noexcept { closeConnections(sharedConnections); }).detach();
    } catch (std::exception const& ex) {
        util::DebugTraceException(std::format(L"[DeviceManager] {0} std::thread fallback failed", context), ex);
        closeConnections(sharedConnections);
    } catch (...) {
        util::DebugTraceUnknownException(std::format(L"[DeviceManager] {0} std::thread fallback failed", context));
        closeConnections(sharedConnections);
    }
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DeviceManager::DeviceManager() : m_discoveryService(std::make_shared<DeviceDiscoveryService>()) {}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceManager::StartDeviceWatcher() {
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        m_reconnectController.AllowReconnects();
    }
    EnsureDiscoveryEventHandlers();
    m_discoveryService->Start();
}

void DeviceManager::StopDeviceWatcher() {
    StopConnectionHeartbeat();
    if (m_discoveryService) m_discoveryService->Stop();
}

void DeviceManager::ShutdownForProcessExit() noexcept {
    try {
        {
            auto guard = m_lock.lock_exclusive();
            m_shutdownForProcessExit = true;
            m_reconnectController.CancelPendingReconnects();
            for (auto& entry : m_connectAttemptIds) {
                ++entry.second;
            }
            for (auto& [id, generation] : m_operationGenerations) {
                (void)id;
                ++generation;
            }
            for (auto& [id, barrier] : m_closeBarriers) {
                (void)id;
                if (barrier) barrier->Completed.SetEvent();
            }
            m_closeBarriers.clear();
        }

        StopDeviceWatcher();

        struct ConnectionForShutdown {
            winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
            winrt::event_token StateChangedToken{};
        };
        std::vector<ConnectionForShutdown> connections;
        {
            auto guard = m_lock.lock_exclusive();
            auto allConnections = m_sessions.ExtractAllConnections();
            connections.reserve(allConnections.size());

            for (auto& [id, info] : allConnections) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            m_sessions.Clear();
            m_reconnectController.ClearTracking();
            m_connectAttemptIds.clear();
            m_operationGenerations.clear();
            m_userActionCascadeIds.clear();
            m_reconnectOnConnectionLossPred = nullptr;
        }
        if (m_discoveryService) {
            m_discoveryService->ClearCache();
        }

        if (!connections.empty()) {
            DebugTrace(L"[DeviceManager] Shutdown detached {0} connection object(s) for process teardown",
                       connections.size());
            for (auto& item : connections) {
                if (item.Connection && item.StateChangedToken.value != 0) {
                    AudioConnectionService::RevokeStateChanged(item.Connection, item.StateChangedToken);
                }
                AudioConnectionService::DetachForProcessExit(item.Connection);
            }
        }
    } catch (...) {
        DebugTrace(L"[DeviceManager] ShutdownForProcessExit ERROR: ignored exception during shutdown");
    }
}

void DeviceManager::SuspendForPowerTransition() noexcept {
    try {
        DebugTrace(L"[DeviceManager] Power transition suspend started");
        StopDeviceWatcher();
        CancelPendingReconnects();

        struct ConnectionForSuspend {
            winrt::hstring DeviceId;
            winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
            winrt::event_token StateChangedToken{};
            std::shared_ptr<CloseBarrier> Barrier;
        };
        std::vector<ConnectionForSuspend> connections;
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) return;
            m_powerTransitionSuspended = true;
            for (auto& entry : m_connectAttemptIds) {
                ++entry.second;
            }
            for (auto& [id, generation] : m_operationGenerations) {
                (void)id;
                ++generation;
            }
            auto allConnections = m_sessions.ExtractAllConnections();
            connections.reserve(allConnections.size());

            for (auto& [id, info] : allConnections) {
                if (info.Connection) {
                    auto deviceId = winrt::hstring(id);
                    connections.push_back({deviceId,
                                           std::move(info.Connection),
                                           info.StateChangedToken,
                                           InstallCloseBarrierLocked(deviceId)});
                }
            }

            m_sessions.Clear();
            for (auto const& [id, barrier] : m_closeBarriers) {
                (void)barrier;
                m_sessions.MarkDisconnecting(winrt::hstring(id));
            }
            m_reconnectController.ClearTracking();
            m_userActionCascadeIds.clear();
        }

        for (auto& item : connections) {
            if (item.StateChangedToken.value != 0) {
                AudioConnectionService::RevokeStateChanged(item.Connection, item.StateChangedToken);
            }
            auto weak = weak_from_this();
            std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> closeConnections;
            closeConnections.push_back(std::move(item.Connection));
            CloseConnectionsOnBackgroundThread(
                std::move(closeConnections),
                L"Power transition cleanup",
                [weak, id = std::move(item.DeviceId), barrier = std::move(item.Barrier)]() mutable {
                    if (auto self = weak.lock()) {
                        self->CompleteCloseBarrierDetached(std::move(id), std::move(barrier), false);
                    } else if (barrier) {
                        barrier->Completed.SetEvent();
                    }
                });
        }

        LogConnectionSnapshot(L"power-suspend");
    } catch (...) {
        DebugTrace(L"[DeviceManager] SuspendForPowerTransition ERROR: ignored exception during suspend");
    }
}

void DeviceManager::ResumeAfterPowerTransition() {
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        m_powerTransitionSuspended = false;
        m_reconnectController.AllowReconnects();
        m_reconnectController.ClearTracking();
        m_userActionCascadeIds.clear();
    }
    DebugTrace(L"[DeviceManager] Power transition resume completed");
    StartDeviceWatcher();
}

void DeviceManager::CancelPendingReconnects() {
    auto guard = m_lock.lock_exclusive();
    m_reconnectController.CancelPendingReconnects();
    m_userActionCascadeIds.clear();
}

void DeviceManager::SetReconnectOnConnectionLossPredicate(ReconnectOnConnectionLossPredicate pred) {
    auto guard = m_lock.lock_exclusive();
    m_reconnectOnConnectionLossPred = std::move(pred);
}

void DeviceManager::SetIncomingConnectionsEnabled(bool enabled) {
    std::vector<std::pair<winrt::hstring, bool>> sessionsToDisable;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_incomingConnectionsEnabled == enabled) return;
        m_incomingConnectionsEnabled = enabled;
        for (auto const& [id, info] : m_sessions.GetConnectionsSnapshot()) {
            if (enabled) {
                m_sessions.SetAcceptIncomingConnections(winrt::hstring(id), true);
            } else {
                if (info.AcceptIncomingConnections) {
                    sessionsToDisable.emplace_back(winrt::hstring(id), info.IsOpen);
                }
                m_sessions.SetAcceptIncomingConnections(winrt::hstring(id), false);
            }
        }
    }

    if (enabled) {
        DebugTrace(L"[DeviceManager] Incoming connections enabled");
        EnableIncomingConnectionsForDiscoveredDevicesDetached();
        return;
    }

    DebugTrace(L"[DeviceManager] Incoming connections disabled");
    for (auto const& [id, wasOpen] : sessionsToDisable) {
        Disconnect(id, wasOpen ? DisconnectReason::UserInitiated : DisconnectReason::Cleanup);
    }
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    OperationToken operation;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) {
            DebugTrace(L"[DeviceManager] ConnectAsync ignored during process exit: {0}", std::wstring(deviceId));
            co_return;
        }
        if (m_powerTransitionSuspended) {
            DebugTrace(L"[DeviceManager] ConnectAsync ignored during power transition suspend: {0}",
                       std::wstring(deviceId));
            co_return;
        }
        m_reconnectController.AllowReconnects();
        m_reconnectController.BeginManualOperation(deviceId);
        operation = BeginOperationLocked(deviceId, ConnectionIntent::ManualConnect);
    }

    (void)co_await ConnectWithIntentAsync(std::move(deviceId), std::move(operation));
}

winrt::Windows::Foundation::IAsyncOperation<bool>
DeviceManager::ConnectWithIntentAsync(winrt::hstring deviceId,
                                      // The coroutine frame must own the token across suspension points.
                                      // cppcheck-suppress passedByValue
                                      OperationToken operation) {
    auto lifetime = shared_from_this();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    const bool reportFailures = operation.Intent != ConnectionIntent::AutoReconnect;
    try {
        bool openEnabledIncomingConnection = false;
        {
            auto guard = m_lock.lock_exclusive();
            if (!IsOperationCurrentLocked(operation)) co_return false;
            if (auto info = m_sessions.FindConnection(deviceId)) {
                if (!info->IsOpen && info->AcceptIncomingConnections &&
                    operation.Intent != ConnectionIntent::IncomingEnable) {
                    openEnabledIncomingConnection = true;
                } else {
                    co_return info->IsOpen;
                }
            }
            if (!openEnabledIncomingConnection &&
                (m_sessions.IsConnecting(deviceId) ||
                 (m_sessions.IsDisconnecting(deviceId) && !m_closeBarriers.contains(deviceIdKey)))) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored; connect already running for {0}",
                           std::wstring(deviceId));
                co_return false;
            }
            if (!openEnabledIncomingConnection) {
                m_sessions.MarkConnecting(deviceId);
            }
        }

        if (openEnabledIncomingConnection) {
            DebugTrace(L"[DeviceManager] ConnectAsync opening enabled incoming connection: {0}",
                       std::wstring(deviceId));
            co_return co_await ReconnectWithIntentAsync(deviceId, operation);
        }
        DebugTrace(L"[DeviceManager] ConnectAsync requested: {0}", std::wstring(deviceId));

        auto clearConnecting = wil::scope_exit([this, deviceId, operation]() noexcept {
            try {
                bool changed = false;
                {
                    auto guard = m_lock.lock_exclusive();
                    if (IsOperationCurrentLocked(operation) && m_sessions.IsConnecting(deviceId)) {
                        m_sessions.UnmarkConnecting(deviceId);
                        changed = true;
                    }
                }
                if (changed) DeviceActivityChanged(deviceId);
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] ConnectWithIntentAsync cleanup ignored exception");
            }
        });

        if (!(co_await WaitForCloseBarrierAsync(operation, operation.Intent == ConnectionIntent::AutoReconnect))) {
            if (reportFailures && IsOperationCurrent(operation)) {
                ReportAsyncConnectionError(*this, deviceId, winrt::hstring(_("UnknownError")), L"close barrier");
            }
            co_return false;
        }
        if (!IsOperationCurrent(operation)) co_return false;

        const bool knownDeviceId = m_discoveryService && m_discoveryService->ContainsDeviceId(deviceIdKey);
        auto targetDevice =
            co_await winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(deviceId);
        if (!IsOperationCurrent(operation)) co_return false;

        if (targetDevice) {
            co_return co_await ConnectInternalAsync(targetDevice, true, operation, reportFailures);
        }

        if (knownDeviceId) {
            DebugTrace(L"[DeviceManager] ConnectAsync known ID could not be resolved: {0}", std::wstring(deviceId));
        }

        if (reportFailures && IsOperationCurrent(operation)) {
            ConnectionError(deviceId, winrt::hstring(_("UnknownError")));
            DeviceStatusChanged(deviceId,
                                winrt::hstring(_("UnknownError")),
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
        co_return false;
    } catch (winrt::hresult_error const& ex) {
        if (reportFailures && IsOperationCurrent(operation)) {
            ConnectionError(deviceId, ex.message());
            DeviceStatusChanged(deviceId,
                                ex.message(),
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
    } catch (std::exception const& ex) {
        auto message = winrt::hstring(util::Utf8ToUtf16(ex.what()));
        if (reportFailures && IsOperationCurrent(operation)) {
            ConnectionError(deviceId, message);
            DeviceStatusChanged(deviceId,
                                message,
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ConnectAsync ERROR");
        auto message = winrt::hstring(_("UnknownError"));
        if (reportFailures && IsOperationCurrent(operation)) {
            ConnectionError(deviceId, message);
            DeviceStatusChanged(deviceId,
                                message,
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
    }
    co_return false;
}

void DeviceManager::ConnectDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        try {
            if (auto self = weak.lock()) {
                co_await self->ConnectAsync(id);
            }
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] ConnectDetached ignored exception");
        }
    }(std::move(weak), std::move(deviceId));
}

winrt::Windows::Foundation::IAsyncAction
DeviceManager::EnableIncomingConnectionAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto lifetime = shared_from_this();
    if (!device) co_return;

    auto deviceId = device.Id();
    OperationToken operation;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit || m_powerTransitionSuspended || !m_incomingConnectionsEnabled ||
            m_sessions.HasConnection(deviceId) || m_sessions.IsConnecting(deviceId) ||
            m_sessions.IsReconnecting(deviceId) || m_sessions.IsDisconnecting(deviceId) ||
            m_closeBarriers.contains(std::wstring(deviceId))) {
            co_return;
        }
        operation = BeginOperationLocked(deviceId, ConnectionIntent::IncomingEnable);
        m_sessions.MarkConnecting(deviceId);
    }

    auto clearConnecting = wil::scope_exit([this, deviceId, operation]() noexcept {
        try {
            bool changed = false;
            {
                auto guard = m_lock.lock_exclusive();
                if (IsOperationCurrentLocked(operation) && m_sessions.IsConnecting(deviceId)) {
                    m_sessions.UnmarkConnecting(deviceId);
                    changed = true;
                }
            }
            if (changed) DeviceActivityChanged(deviceId);
        } catch (...) {
            util::DebugTraceUnknownException(
                L"[DeviceManager] EnableIncomingConnectionAsync clear connecting ignored exception");
        }
    });

    (void)co_await ConnectInternalAsync(device, false, std::move(operation), true);
}

void DeviceManager::EnableIncomingConnectionDetached(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak,
       winrt::Windows::Devices::Enumeration::DeviceInformation target) -> winrt::fire_and_forget {
        try {
            if (auto self = weak.lock()) {
                co_await self->EnableIncomingConnectionAsync(std::move(target));
            }
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] EnableIncomingConnectionDetached ignored exception");
        }
    }(std::move(weak), std::move(device));
}

void DeviceManager::EnableIncomingConnectionsForDiscoveredDevicesDetached() {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak) -> winrt::fire_and_forget {
        try {
            auto self = weak.lock();
            if (!self || !self->m_discoveryService) co_return;
            auto devices = co_await self->m_discoveryService->RefreshAsync();
            for (auto const& device : devices) {
                {
                    auto guard = self->m_lock.lock_shared();
                    if (self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                        !self->m_incomingConnectionsEnabled) {
                        co_return;
                    }
                }
                co_await self->EnableIncomingConnectionAsync(device);
            }
        } catch (...) {
            util::DebugTraceUnknownException(
                L"[DeviceManager] EnableIncomingConnectionsForDiscoveredDevicesDetached ignored exception");
        }
    }(std::move(weak));
}

void DeviceManager::ReenableIncomingConnectionDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        try {
            auto self = weak.lock();
            if (!self || !self->m_discoveryService) co_return;
            {
                auto guard = self->m_lock.lock_shared();
                if (self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                    !self->m_incomingConnectionsEnabled || self->m_sessions.HasConnection(id) ||
                    self->m_sessions.IsDeviceBusy(id) || self->m_closeBarriers.contains(std::wstring(id))) {
                    co_return;
                }
            }

            auto devices = co_await self->m_discoveryService->RefreshAsync();
            for (auto const& device : devices) {
                if (device.Id() == id) {
                    co_await self->EnableIncomingConnectionAsync(device);
                    co_return;
                }
            }
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] ReenableIncomingConnectionDetached ignored exception");
        }
    }(std::move(weak), std::move(deviceId));
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ReconnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    OperationToken operation;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) {
            DebugTrace(L"[DeviceManager] ReconnectAsync ignored during process exit: {0}", std::wstring(deviceId));
            co_return;
        }
        if (m_powerTransitionSuspended) {
            DebugTrace(L"[DeviceManager] ReconnectAsync ignored during power transition suspend: {0}",
                       std::wstring(deviceId));
            co_return;
        }
        m_reconnectController.AllowReconnects();
        m_reconnectController.BeginManualOperation(deviceId);
        operation = BeginOperationLocked(deviceId, ConnectionIntent::ManualReconnect);
    }

    (void)co_await ReconnectWithIntentAsync(std::move(deviceId), std::move(operation));
}

winrt::Windows::Foundation::IAsyncOperation<bool>
DeviceManager::ReconnectWithIntentAsync(winrt::hstring deviceId,
                                        // The coroutine frame must own the token across suspension points.
                                        // cppcheck-suppress passedByValue
                                        OperationToken operation) {
    auto lifetime = shared_from_this();
    try {
        if (deviceId.empty()) co_return false;
        DebugTrace(L"[DeviceManager] ReconnectAsync requested: {0}", std::wstring(deviceId));

        {
            auto guard = m_lock.lock_exclusive();
            if (!IsOperationCurrentLocked(operation)) co_return false;
            if (m_sessions.IsReconnecting(deviceId)) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored; reconnect already running for {0}",
                           std::wstring(deviceId));
                co_return false;
            }
            m_sessions.MarkReconnecting(deviceId);
        }

        auto clearReconnecting = wil::scope_exit([this, deviceId, operation]() noexcept {
            try {
                bool changed = false;
                {
                    auto guard = m_lock.lock_exclusive();
                    if (IsOperationCurrentLocked(operation) && m_sessions.IsReconnecting(deviceId)) {
                        m_sessions.UnmarkReconnecting(deviceId);
                        changed = true;
                    }
                }
                if (changed) DeviceActivityChanged(deviceId);
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] ReconnectWithIntentAsync cleanup ignored exception");
            }
        });

        DeviceStatusChanged(
            deviceId,
            winrt::hstring(_("Reconnecting")),
            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton,
            DeviceStatusKind::Reconnecting);

        // Extract the old connection and close it synchronously (on a background
        // thread) BEFORE creating the new one. If we let Close() run in parallel
        // with the new Connect (as the old zombie-list path did), the BT stack
        // sees an overlap and closes the newly opened connection immediately,
        // which then looks like an Unexpected disconnect and triggers a spurious
        // auto-reconnect cycle.
        winrt::Windows::Media::Audio::AudioPlaybackConnection oldConn{nullptr};
        winrt::event_token oldToken{};
        std::shared_ptr<CloseBarrier> closeBarrier;
        {
            auto guard = m_lock.lock_exclusive();
            if (!IsOperationCurrentLocked(operation)) co_return false;
            auto extracted = m_sessions.ExtractConnection(deviceId);
            if (extracted) {
                oldConn = std::move(extracted->Connection);
                oldToken = extracted->StateChangedToken;
                ++m_connectAttemptIds[std::wstring(deviceId)];
                closeBarrier = InstallCloseBarrierLocked(deviceId);
                TrackUserActionCascadeLocked(deviceId);
            }
        }

        if (oldConn) {
            // Revoke the event token first so no stale Closed callbacks fire.
            if (oldToken.value != 0) {
                AudioConnectionService::RevokeStateChanged(oldConn, oldToken);
            }
            DebugTrace(L"[DeviceManager] ReconnectAsync closing old connection: {0}", std::wstring(deviceId));
            auto weak = weak_from_this();
            std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> connections;
            connections.push_back(std::move(oldConn));
            CloseConnectionsOnBackgroundThread(
                std::move(connections), L"Reconnect cleanup", [weak, id = deviceId, barrier = closeBarrier]() mutable {
                    if (auto self = weak.lock()) {
                        self->CompleteCloseBarrierDetached(std::move(id), std::move(barrier), false);
                    } else if (barrier) {
                        barrier->Completed.SetEvent();
                    }
                });
        }

        if (!(co_await WaitForCloseBarrierAsync(operation, operation.Intent == ConnectionIntent::AutoReconnect))) {
            co_return false;
        }
        if (!IsOperationCurrent(operation)) co_return false;
        co_return co_await ConnectWithIntentAsync(deviceId, operation);
    } catch (winrt::hresult_error const& ex) {
        if (operation.Intent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
            ReportAsyncConnectionError(*this, deviceId, ex.message(), L"ReconnectAsync");
        }
    } catch (std::exception const& ex) {
        if (operation.Intent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
            ReportAsyncConnectionError(
                *this, deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), L"ReconnectAsync");
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ReconnectAsync ERROR");
        if (operation.Intent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
            ReportAsyncConnectionError(*this, deviceId, winrt::hstring(_("UnknownError")), L"ReconnectAsync");
        }
    }
    co_return false;
}

void DeviceManager::ReconnectDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        try {
            if (auto self = weak.lock()) {
                co_await self->ReconnectAsync(id);
            }
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] ReconnectDetached ignored exception");
        }
    }(std::move(weak), std::move(deviceId));
}

void DeviceManager::Disconnect(winrt::hstring deviceId) {
    Disconnect(std::move(deviceId), DisconnectReason::UserInitiated, false);
}

void DeviceManager::DisconnectAll() {
    std::unordered_set<std::wstring> affectedIds;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit || m_powerTransitionSuspended) return;
        m_reconnectController.CancelPendingReconnects();
        m_userActionCascadeIds.clear();
        for (auto& [id, generation] : m_operationGenerations) {
            ++generation;
            affectedIds.insert(id);
        }
        auto allConnections = m_sessions.GetConnectionsSnapshot();
        for (auto const& [id, info] : allConnections) {
            (void)info;
            affectedIds.insert(id);
        }
    }
    for (auto const& id : affectedIds) {
        Disconnect(winrt::hstring(id), DisconnectReason::UserInitiated, true);
    }
}

void DeviceManager::ReconnectAll() {
    std::vector<std::wstring> connectedIds;
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit || m_powerTransitionSuspended) return;
        auto allConnections = m_sessions.GetConnectionsSnapshot();
        for (auto const& [id, info] : allConnections) {
            if (info.IsOpen && !m_sessions.IsDisconnecting(winrt::hstring(id)) &&
                !m_sessions.IsReconnecting(winrt::hstring(id))) {
                connectedIds.push_back(id);
            }
        }
    }
    for (auto const& id : connectedIds) {
        ReconnectDetached(winrt::hstring(id));
    }
}

void DeviceManager::SetReconnectOnConnectionLoss(winrt::hstring deviceId, bool enabled) {
    bool cancelActiveAttempt = false;
    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.SetReconnectOnConnectionLoss(deviceId, enabled);
        if (!enabled) {
            cancelActiveAttempt = m_reconnectController.HasAttemptInProgress(deviceId);
            m_reconnectController.CancelDevice(deviceId);
            if (cancelActiveAttempt) {
                InvalidateDeviceOperationLocked(deviceId);
                ++m_connectAttemptIds[std::wstring(deviceId)];
                m_sessions.UnmarkReconnecting(deviceId);
            }
        }
    }
    if (cancelActiveAttempt) Disconnect(std::move(deviceId), DisconnectReason::Cleanup);
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
DeviceManager::RefreshDevicesAsync() {
    auto lifetime = shared_from_this();
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit || !m_discoveryService) {
            co_return nullptr;
        }
    }

    co_return co_await m_discoveryService->RefreshAsync();
}

std::vector<DeviceConnectionInfo> DeviceManager::GetConnectedDevices() const {
    auto guard = m_lock.lock_shared();
    return m_sessions.ConnectedDevices();
}

std::vector<DeviceConnectionInfo> DeviceManager::GetConnectionSessions() const {
    auto guard = m_lock.lock_shared();
    std::vector<DeviceConnectionInfo> result;
    for (auto const& [id, info] : m_sessions.GetConnectionsSnapshot()) {
        (void)id;
        result.push_back(info);
    }
    return result;
}

bool DeviceManager::IsDeviceConnected(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    auto info = m_sessions.FindConnection(deviceId);
    return info && info->IsOpen;
}

std::optional<std::wstring> DeviceManager::GetConnectionDisplayName(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    auto info = m_sessions.FindConnection(deviceId);
    if (!info || !info->IsOpen) return std::nullopt;
    if (!info->Name.empty()) return info->Name;
    if (!info->Id.empty()) return info->Id;
    return std::wstring(deviceId);
}

bool DeviceManager::HasConnections() const {
    auto guard = m_lock.lock_shared();
    return m_sessions.HasConnections();
}

bool DeviceManager::HasBusyOperations() const {
    auto guard = m_lock.lock_shared();
    return m_sessions.HasBusyOperations() || m_reconnectController.HasPendingTimers();
}

bool DeviceManager::IsDeviceBusy(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_sessions.IsDeviceBusy(deviceId) || m_reconnectController.HasPendingTimer(deviceId);
}

apc::device_picker::DeviceActivitySnapshot DeviceManager::GetDevicePickerActivitySnapshot() const {
    auto guard = m_lock.lock_shared();
    auto result = m_sessions.GetDevicePickerActivitySnapshot();
    for (auto& id : m_reconnectController.PendingDeviceIds()) {
        result.BusyIds.insert(std::move(id));
    }
    return result;
}

apc::device_picker::DeviceInventorySnapshot DeviceManager::GetDevicePickerInventorySnapshot() const {
    auto discoveryService = m_discoveryService;
    return discoveryService ? discoveryService->GetInventorySnapshot() : apc::device_picker::DeviceInventorySnapshot{};
}

DeviceTrayPresentationSnapshot DeviceManager::GetTrayPresentationSnapshot() const {
    auto guard = m_lock.lock_shared();
    DeviceTrayPresentationSnapshot snapshot;
    snapshot.ConnectedDevices = m_sessions.ConnectedDevices();
    snapshot.HasBusyOperations = m_sessions.HasBusyOperations() || m_reconnectController.HasPendingTimers();
    return snapshot;
}

void DeviceManager::StartConnectionHeartbeat() {
    bool started = false;
    try {
        std::lock_guard lock(m_heartbeatTimerMutex);
        if (m_heartbeatTimer) return;

        auto weak = weak_from_this();
        m_heartbeatTimer = winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
            [weak](auto) {
                if (auto self = weak.lock()) {
                    self->LogConnectionSnapshot(L"heartbeat");
                }
            },
            std::chrono::minutes(c_heartbeatIntervalMinutes));
        started = true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DeviceManager] Heartbeat timer creation failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DeviceManager] Heartbeat timer creation failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] Heartbeat timer creation failed");
    }

    if (started) {
        LogConnectionSnapshot(L"heartbeat-started");
    }
}

void DeviceManager::StopConnectionHeartbeat() {
    winrt::Windows::System::Threading::ThreadPoolTimer timer{nullptr};
    {
        std::lock_guard lock(m_heartbeatTimerMutex);
        timer = std::exchange(m_heartbeatTimer, nullptr);
    }
    if (timer) {
        try {
            timer.Cancel();
        } catch (...) {
        }
        LogConnectionSnapshot(L"heartbeat-stopped");
    }
}

void DeviceManager::LogConnectionSnapshot(winrt::hstring const& reason) const {
    std::vector<DeviceManagerDiagnosticSnapshot> snapshots;
    bool allReconnectsCancelled = false;
    std::size_t deviceCacheSize = m_discoveryService ? m_discoveryService->CacheSize() : 0;
    std::vector<std::pair<std::wstring, DeviceConnectionInfo>> allConnections;
    {
        auto guard = m_lock.lock_shared();
        allConnections = m_sessions.GetConnectionsSnapshot();
        snapshots.reserve(allConnections.size());
        allReconnectsCancelled = m_reconnectController.AllReconnectsCancelled();
        for (auto const& [id, info] : allConnections) {
            DeviceManagerDiagnosticSnapshot snapshot;
            snapshot.Id = winrt::hstring(id);
            snapshot.Name = !info.Name.empty() ? info.Name : id;
            snapshot.HasConnection = static_cast<bool>(info.Connection);
            snapshot.IsOpen = info.IsOpen;
            snapshot.ReconnectOnConnectionLoss = info.ReconnectOnConnectionLoss;
            snapshot.AcceptIncomingConnections = info.AcceptIncomingConnections;
            snapshot.Disconnecting = m_sessions.IsDisconnecting(snapshot.Id);
            snapshot.Reconnecting = m_sessions.IsReconnecting(snapshot.Id);
            snapshot.CancelledReconnect = m_reconnectController.IsCancelled(snapshot.Id);
            snapshot.ReconnectAttempts = m_reconnectController.Attempts(snapshot.Id);
            if (auto iter = m_connectAttemptIds.find(id); iter != m_connectAttemptIds.end()) {
                snapshot.ConnectAttemptId = iter->second;
            }
            snapshots.push_back(std::move(snapshot));
        }
    }

    LogDeviceManagerDiagnosticSnapshot(reason, deviceCacheSize, allReconnectsCancelled, snapshots);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceManager::Disconnect(winrt::hstring deviceId, DisconnectReason reason) {
    Disconnect(std::move(deviceId), reason, false);
}

void DeviceManager::Disconnect(winrt::hstring deviceId, DisconnectReason reason, bool suppressCascade) {
    auto reasonName = [](DisconnectReason value) -> std::wstring_view {
        switch (value) {
            case DisconnectReason::UserInitiated: return L"UserInitiated";
            case DisconnectReason::UserInitiatedCascade: return L"UserInitiatedCascade";
            case DisconnectReason::Unexpected: return L"Unexpected";
            case DisconnectReason::Cleanup: return L"Cleanup";
            default: return L"UnknownReason";
        }
    };
    DebugTrace(L"[DeviceManager] Disconnect requested: id={0} reason={1} suppressCascade={2}",
               std::wstring(deviceId),
               reasonName(reason),
               suppressCascade);

    bool reconnectOnConnectionLoss = false;
    bool acceptIncomingConnections = false;
    bool noActiveConnections = false;
    bool stateChanged = false;
    winrt::Windows::Media::Audio::AudioPlaybackConnection connection{nullptr};
    winrt::event_token stateChangedToken{};
    std::shared_ptr<CloseBarrier> closeBarrier;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;

        InvalidateDeviceOperationLocked(deviceId);
        ++m_connectAttemptIds[std::wstring(deviceId)];
        m_sessions.UnmarkConnecting(deviceId);
        m_sessions.UnmarkReconnecting(deviceId);
        if (reason == DisconnectReason::UserInitiated) {
            m_reconnectController.CancelDevice(deviceId);
        }

        auto extracted = m_sessions.ExtractConnection(deviceId);
        if (!extracted) {
            stateChanged = true;
        } else {
            connection = std::move(extracted->Connection);
            stateChangedToken = extracted->StateChangedToken;
            reconnectOnConnectionLoss = extracted->ReconnectOnConnectionLoss;
            acceptIncomingConnections = extracted->AcceptIncomingConnections;
            if (m_powerTransitionSuspended) {
                reconnectOnConnectionLoss = false;
                acceptIncomingConnections = false;
            }
            noActiveConnections = !m_sessions.HasConnections();
            closeBarrier = InstallCloseBarrierLocked(deviceId);
            stateChanged = true;
            if (reason == DisconnectReason::UserInitiated && !suppressCascade) {
                TrackUserActionCascadeLocked(deviceId);
            }
        }
    }

    if (!closeBarrier) {
        if (stateChanged) DeviceActivityChanged(deviceId);
        return;
    }

    if (noActiveConnections) {
        StopConnectionHeartbeat();
    }

    // Revoke the StateChanged token so the zombie cannot fire events at us.
    if (connection && stateChangedToken.value != 0) {
        AudioConnectionService::RevokeStateChanged(connection, stateChangedToken);
    }

    bool powerTransitionSuspended = false;
    {
        auto guard = m_lock.lock_shared();
        powerTransitionSuspended = m_powerTransitionSuspended;
    }

    if (reason != DisconnectReason::Cleanup && !powerTransitionSuspended) {
        if (reason == DisconnectReason::UserInitiatedCascade) {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            if (reconnectOnConnectionLoss) {
                ScheduleReconnect(deviceId);
            }
        } else {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            if (reason == DisconnectReason::Unexpected && reconnectOnConnectionLoss) {
                ScheduleReconnect(deviceId);
            }
        }
    }

    DeviceActivityChanged(deviceId);

    bool restoreIncoming = false;
    {
        auto guard = m_lock.lock_shared();
        restoreIncoming = acceptIncomingConnections && m_incomingConnectionsEnabled &&
                          (reason == DisconnectReason::UserInitiated || reason == DisconnectReason::Cleanup);
    }

    auto weak = weak_from_this();
    if (connection) {
        std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> connections;
        connections.push_back(std::move(connection));
        CloseConnectionsOnBackgroundThread(std::move(connections),
                                           L"Disconnect cleanup",
                                           [weak, id = deviceId, barrier = closeBarrier, restoreIncoming]() mutable {
                                               if (auto self = weak.lock()) {
                                                   self->CompleteCloseBarrierDetached(
                                                       std::move(id), std::move(barrier), restoreIncoming);
                                               } else if (barrier) {
                                                   barrier->Completed.SetEvent();
                                               }
                                           });
    } else {
        CompleteCloseBarrierDetached(deviceId, std::move(closeBarrier), restoreIncoming);
    }

    LogConnectionSnapshot(winrt::hstring(L"disconnect:") + winrt::hstring(reasonName(reason)));
}

void DeviceManager::ReportConnectionFailure(winrt::hstring const& deviceId,
                                            winrt::hstring const& message,
                                            bool cleanupConnection) {
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }
    ConnectionError(deviceId, message);
    if (cleanupConnection) {
        Disconnect(deviceId, DisconnectReason::Cleanup);
    }
    DeviceStatusChanged(deviceId,
                        message,
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                        DeviceStatusKind::Error);
}

winrt::Windows::Foundation::IAsyncOperation<bool>
DeviceManager::ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device,
                                    bool openImmediately,
                                    // The coroutine frame must own the token across suspension points.
                                    // cppcheck-suppress passedByValue
                                    OperationToken operation,
                                    bool reportFailures) {
    auto lifetime = shared_from_this();
    auto deviceId = device.Id();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    std::size_t attemptId = 0;

    DeviceConnectionInfo metadataTemplate;
    PopulateConnectionMetadata(metadataTemplate, device);

    {
        auto guard = m_lock.lock_exclusive();
        if (!IsOperationCurrentLocked(operation)) co_return false;
        if (m_sessions.HasConnection(deviceId)) co_return false;
        attemptId = ++m_connectAttemptIds[deviceIdKey];
    }

    // Create the connection on a background thread. If AudioPlaybackConnection is STA-bound,
    // this gives it its own COM apartment instead of marshalling everything to the UI thread.
    co_await winrt::resume_background();

    try {
        {
            auto guard = m_lock.lock_shared();
            if (!IsOperationCurrentLocked(operation)) co_return false;
        }

        auto closePendingConnectionForRetry =
            [&](winrt::Windows::Media::Audio::AudioPlaybackConnection& failedConnection) -> bool {
            std::optional<DeviceConnectionInfo> extracted;
            {
                auto guard = m_lock.lock_exclusive();
                auto attempt = m_connectAttemptIds.find(deviceIdKey);
                if (!IsOperationCurrentLocked(operation) || attempt == m_connectAttemptIds.end() ||
                    attempt->second != attemptId) {
                    return false;
                }
                extracted = m_sessions.ExtractConnection(deviceId);
            }

            if (extracted) {
                if (extracted->StateChangedToken.value != 0) {
                    AudioConnectionService::RevokeStateChanged(extracted->Connection, extracted->StateChangedToken);
                }
                AudioConnectionService::Close(extracted->Connection);
            }
            failedConnection = nullptr;
            LogConnectionSnapshot(L"open-retry-cleanup");
            return true;
        };

        for (std::size_t openAttempt = 1; openAttempt <= c_openTransientFailureMaxAttempts; ++openAttempt) {
            if (!IsOperationCurrent(operation)) co_return false;
            auto connection = AudioConnectionService::TryCreateFromId(deviceId);
            auto detachConnectionOnExitShutdown = wil::scope_exit([&]() noexcept {
                auto guard = m_lock.lock_shared();
                if (m_shutdownForProcessExit) {
                    AudioConnectionService::DetachForProcessExit(connection);
                }
            });

            if (!connection) {
                // Do NOT touch the reconnecting flag here. ReconnectAsync owns it
                // for the entire reconnect flow and will clear it when finished.
                DebugTrace(L"[DeviceManager] TryCreateFromId returned null: {0}", std::wstring(deviceId));
                bool shutdownForProcessExit = false;
                {
                    auto guard = m_lock.lock_shared();
                    shutdownForProcessExit = m_shutdownForProcessExit;
                }
                if (!shutdownForProcessExit) {
                    if (reportFailures) {
                        ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), false);
                    }
                    bool restoreIncoming = false;
                    {
                        auto guard = m_lock.lock_shared();
                        restoreIncoming = openImmediately && m_incomingConnectionsEnabled;
                    }
                    if (restoreIncoming) ReenableIncomingConnectionDetached(deviceId);
                }
                co_return false;
            }

            DeviceConnectionInfo info;
            info.Id = metadataTemplate.Id.empty() ? deviceIdKey : metadataTemplate.Id;
            info.Name = metadataTemplate.Name.empty() ? info.Id : metadataTemplate.Name;
            info.Connection = connection;
            info.IsOpen = false;
            auto weak = weak_from_this();
            auto stateChangedDeviceId = deviceId;
            info.StateChangedToken = AudioConnectionService::RegisterStateChanged(
                connection, [weak, stateChangedDeviceId](auto sender, auto args) {
                    if (auto self = weak.lock()) {
                        self->OnConnectionStateChanged(stateChangedDeviceId, sender, args);
                    }
                });
            ReconnectOnConnectionLossPredicate pred;
            bool acceptIncomingConnections = false;
            {
                auto guard = m_lock.lock_shared();
                pred = m_reconnectOnConnectionLossPred;
                acceptIncomingConnections = m_incomingConnectionsEnabled;
            }
            info.ReconnectOnConnectionLoss = pred ? pred(deviceId) : false;
            info.AcceptIncomingConnections = acceptIncomingConnections;

            bool duplicateConnection = false;
            bool shutdownForProcessExit = false;
            {
                auto guard = m_lock.lock_exclusive();
                auto attempt = m_connectAttemptIds.find(deviceIdKey);
                shutdownForProcessExit = m_shutdownForProcessExit;
                if (!IsOperationCurrentLocked(operation) || attempt == m_connectAttemptIds.end() ||
                    attempt->second != attemptId || m_sessions.HasConnection(deviceId)) {
                    duplicateConnection = true;
                } else {
                    m_sessions.InsertOrUpdateConnection(deviceId, std::move(info));
                }
            }
            if (duplicateConnection) {
                if (!shutdownForProcessExit && info.StateChangedToken.value != 0) {
                    AudioConnectionService::RevokeStateChanged(connection, info.StateChangedToken);
                }
                if (shutdownForProcessExit) {
                    AudioConnectionService::DetachForProcessExit(connection);
                } else {
                    AudioConnectionService::Close(connection);
                }
                co_return false;
            }

            bool isReconnecting = false;
            {
                auto guard = m_lock.lock_shared();
                if (!IsOperationCurrentLocked(operation)) co_return false;
                isReconnecting = m_sessions.IsReconnecting(deviceId);
            }

            if (openImmediately && !isReconnecting && operation.Intent != ConnectionIntent::AutoReconnect) {
                DeviceStatusChanged(
                    deviceId,
                    winrt::hstring(_("Connecting")),
                    winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton,
                    DeviceStatusKind::Connecting);
            }

            DebugTrace(L"[DeviceManager] Open attempt {0}/{1} starting: {2}",
                       openAttempt,
                       c_openTransientFailureMaxAttempts,
                       std::wstring(deviceId));
            co_await AudioConnectionService::StartAsync(connection);
            DebugTrace(
                L"[DeviceManager] StartAsync completed: id={0} openAttempt={1}", std::wstring(deviceId), openAttempt);
            if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;

            if (!openImmediately) {
                bool currentAttempt = false;
                bool alreadyOpen = false;
                {
                    auto guard = m_lock.lock_shared();
                    auto attempt = m_connectAttemptIds.find(deviceIdKey);
                    auto currentInfo = m_sessions.FindConnection(deviceId);
                    currentAttempt = IsOperationCurrentLocked(operation) && m_incomingConnectionsEnabled &&
                                     attempt != m_connectAttemptIds.end() && attempt->second == attemptId &&
                                     currentInfo && currentInfo->Connection == connection;
                    alreadyOpen = currentAttempt && currentInfo->IsOpen;
                }
                if (!currentAttempt) {
                    Disconnect(deviceId, DisconnectReason::Cleanup);
                    co_return false;
                }

                DebugTrace(L"[DeviceManager] Incoming connection enabled: {0}", std::wstring(deviceId));
                if (!alreadyOpen) {
                    DeviceStatusChanged(deviceId,
                                        winrt::hstring(_("ReadyForConnection")),
                                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                        DeviceStatusKind::Ready);
                }
                LogConnectionSnapshot(L"incoming-enabled");
                co_return true;
            }

            auto result = co_await AudioConnectionService::OpenAsync(connection);
            DebugTrace(L"[DeviceManager] OpenAsync completed: id={0} openAttempt={1} status={2} extended=0x{3:08X}",
                       std::wstring(deviceId),
                       openAttempt,
                       DeviceOpenResultStatusName(result.Status()),
                       static_cast<uint32_t>(result.ExtendedError()));
            if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;

            bool currentAttempt = false;
            {
                auto guard = m_lock.lock_exclusive();
                auto attempt = m_connectAttemptIds.find(deviceIdKey);
                currentAttempt = IsOperationCurrentLocked(operation) && attempt != m_connectAttemptIds.end() &&
                                 attempt->second == attemptId && m_sessions.HasConnection(deviceId);
            }
            if (!currentAttempt) co_return false;

            switch (result.Status()) {
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: {
                    bool becameOpen = false;
                    {
                        auto guard = m_lock.lock_exclusive();
                        auto existingInfo = m_sessions.FindConnection(deviceId);
                        auto attempt = m_connectAttemptIds.find(deviceIdKey);
                        if (!IsOperationCurrentLocked(operation) || !existingInfo ||
                            attempt == m_connectAttemptIds.end() || attempt->second != attemptId)
                            co_return false;
                        becameOpen = !existingInfo->IsOpen;
                        if (becameOpen) {
                            m_sessions.UpdateConnectionIsOpen(deviceId, true);
                        }
                        m_sessions.UnmarkReconnecting(deviceId);
                        if (operation.Intent != ConnectionIntent::AutoReconnect) {
                            m_reconnectController.CompleteConnectionSucceeded(deviceId);
                        }
                        m_userActionCascadeIds.erase(deviceIdKey);
                    }
                    isReconnecting = false;
                    if (becameOpen) {
                        DeviceConnected(deviceId);
                        DeviceStatusChanged(deviceId,
                                            winrt::hstring(_("Connected")),
                                            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::
                                                ShowDisconnectButton,
                                            DeviceStatusKind::Connected);
                    }
                    LogConnectionSnapshot(L"open-success");
                    StartConnectionHeartbeat();
                    co_return true;
                }
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
                    if (IsRetryableOpenFailure(result.Status(), result.ExtendedError()) &&
                        openAttempt < c_openTransientFailureMaxAttempts) {
                        const auto delay = OpenTransientRetryDelay(openAttempt);
                        DebugTrace(L"[DeviceManager] OpenAsync transient failure; retry scheduled: id={0} "
                                   L"openAttempt={1} delayMs={2} status={3} extended=0x{4:08X}",
                                   std::wstring(deviceId),
                                   openAttempt,
                                   delay.count(),
                                   DeviceOpenResultStatusName(result.Status()),
                                   static_cast<uint32_t>(result.ExtendedError()));
                        if (!closePendingConnectionForRetry(connection)) co_return false;
                        co_await winrt::resume_after(std::max(delay, ReconnectController::ReconnectCloseCooldown()));
                        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
                        continue;
                    }
                    if (reportFailures) {
                        ReportConnectionFailure(deviceId, winrt::hstring(_("RequestTimedOut")), true);
                    } else {
                        Disconnect(deviceId, DisconnectReason::Cleanup);
                    }
                    co_return false;
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
                    if (reportFailures) {
                        ReportConnectionFailure(deviceId, winrt::hstring(_("DeniedBySystem")), true);
                    } else {
                        Disconnect(deviceId, DisconnectReason::Cleanup);
                    }
                    co_return false;
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure: {
                    if (IsRetryableOpenFailure(result.Status(), result.ExtendedError()) &&
                        openAttempt < c_openTransientFailureMaxAttempts) {
                        const auto delay = OpenTransientRetryDelay(openAttempt);
                        DebugTrace(L"[DeviceManager] OpenAsync transient failure; retry scheduled: id={0} "
                                   L"openAttempt={1} delayMs={2} status={3} extended=0x{4:08X}",
                                   std::wstring(deviceId),
                                   openAttempt,
                                   delay.count(),
                                   DeviceOpenResultStatusName(result.Status()),
                                   static_cast<uint32_t>(result.ExtendedError()));
                        if (!closePendingConnectionForRetry(connection)) co_return false;
                        co_await winrt::resume_after(std::max(delay, ReconnectController::ReconnectCloseCooldown()));
                        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
                        continue;
                    }
                    winrt::hresult_error err(result.ExtendedError());
                    if (reportFailures) {
                        ReportConnectionFailure(deviceId, err.message(), true);
                    } else {
                        Disconnect(deviceId, DisconnectReason::Cleanup);
                    }
                    co_return false;
                }
            }
        }
    } catch (winrt::hresult_error const& ex) {
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, ex.message(), true);
        } else {
            Disconnect(deviceId, DisconnectReason::Cleanup);
        }
    } catch (std::exception const& ex) {
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), true);
        } else {
            Disconnect(deviceId, DisconnectReason::Cleanup);
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ConnectInternalAsync ERROR");
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), true);
        } else {
            Disconnect(deviceId, DisconnectReason::Cleanup);
        }
    }
    co_return false;
}

DeviceManager::OperationToken DeviceManager::BeginOperationLocked(winrt::hstring const& deviceId,
                                                                  ConnectionIntent intent) {
    const auto key = std::wstring(deviceId);
    auto& generation = m_operationGenerations[key];
    ++generation;
    if (generation == 0) ++generation;
    return OperationToken{key, generation, intent};
}

void DeviceManager::InvalidateDeviceOperationLocked(winrt::hstring const& deviceId) {
    auto& generation = m_operationGenerations[std::wstring(deviceId)];
    ++generation;
    if (generation == 0) ++generation;
}

bool DeviceManager::IsOperationCurrent(OperationToken const& operation) const {
    auto guard = m_lock.lock_shared();
    return IsOperationCurrentLocked(operation);
}

bool DeviceManager::IsOperationCurrentLocked(OperationToken const& operation) const {
    if (m_shutdownForProcessExit || m_powerTransitionSuspended) return false;
    auto iter = m_operationGenerations.find(operation.DeviceId);
    return iter != m_operationGenerations.end() && iter->second == operation.Generation;
}

bool DeviceManager::IsConnectAttemptCurrent(OperationToken const& operation, std::size_t attemptId) const {
    auto guard = m_lock.lock_shared();
    if (!IsOperationCurrentLocked(operation)) return false;
    auto attempt = m_connectAttemptIds.find(operation.DeviceId);
    return attempt != m_connectAttemptIds.end() && attempt->second == attemptId;
}

winrt::Windows::Foundation::IAsyncOperation<bool> DeviceManager::WaitForCloseBarrierAsync(OperationToken operation,
                                                                                          bool waitIndefinitely) {
    auto lifetime = shared_from_this();
    std::shared_ptr<CloseBarrier> barrier;
    {
        auto guard = m_lock.lock_shared();
        if (!IsOperationCurrentLocked(operation)) co_return false;
        auto iter = m_closeBarriers.find(operation.DeviceId);
        if (iter != m_closeBarriers.end()) barrier = iter->second;
    }

    if (!barrier) co_return true;

    const auto timeout = waitIndefinitely ? winrt::Windows::Foundation::TimeSpan{} : std::chrono::seconds(30);
    const bool signaled = co_await winrt::resume_on_signal(barrier->Completed.get(), timeout);
    if (!signaled) {
        DebugTrace(L"[DeviceManager] Close barrier timed out; new connection remains blocked: {0}", operation.DeviceId);
        co_return false;
    }
    co_return IsOperationCurrent(operation);
}

std::shared_ptr<DeviceManager::CloseBarrier> DeviceManager::InstallCloseBarrierLocked(winrt::hstring const& deviceId) {
    const auto key = std::wstring(deviceId);
    auto existing = m_closeBarriers.find(key);
    if (existing != m_closeBarriers.end()) return existing->second;

    auto generation = m_operationGenerations[key];
    auto barrier = std::make_shared<CloseBarrier>(generation);
    m_closeBarriers.emplace(key, barrier);
    m_sessions.MarkDisconnecting(deviceId);
    return barrier;
}

void DeviceManager::CompleteCloseBarrierDetached(winrt::hstring deviceId,
                                                 std::shared_ptr<CloseBarrier> barrier,
                                                 bool restoreIncoming) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak,
       winrt::hstring id,
       std::shared_ptr<CloseBarrier> closeBarrier,
       bool shouldRestoreIncoming) -> winrt::fire_and_forget {
        try {
            co_await winrt::resume_after(ReconnectController::ReconnectCloseCooldown());
            auto self = weak.lock();
            if (!self) {
                closeBarrier->Completed.SetEvent();
                co_return;
            }

            bool completedCurrentBarrier = false;
            bool reenable = false;
            {
                auto guard = self->m_lock.lock_exclusive();
                auto iter = self->m_closeBarriers.find(std::wstring(id));
                if (iter != self->m_closeBarriers.end() && iter->second == closeBarrier) {
                    self->m_closeBarriers.erase(iter);
                    self->m_sessions.UnmarkDisconnecting(id);
                    completedCurrentBarrier = true;
                    reenable = shouldRestoreIncoming && !self->m_shutdownForProcessExit &&
                               !self->m_powerTransitionSuspended && self->m_incomingConnectionsEnabled;
                }
            }
            if (completedCurrentBarrier) self->DeviceActivityChanged(id);
            closeBarrier->Completed.SetEvent();
            if (reenable) self->ReenableIncomingConnectionDetached(std::move(id));
        } catch (...) {
            closeBarrier->Completed.SetEvent();
            util::DebugTraceUnknownException(L"[DeviceManager] Close barrier completion ignored exception");
        }
    }(std::move(weak), std::move(deviceId), std::move(barrier), restoreIncoming);
}

void DeviceManager::TrackUserActionCascadeLocked(winrt::hstring const& deviceId) {
    auto now = std::chrono::steady_clock::now();
    PruneUserActionCascadeLocked(now);

    bool marked = false;
    auto const expiresAt = now + c_userActionCascadeWindow;
    auto connections = m_sessions.GetConnectionsSnapshot();
    for (auto const& [id, info] : connections) {
        if (id == std::wstring(deviceId) || !info.IsOpen) continue;
        m_userActionCascadeIds[id] = expiresAt;
        marked = true;
    }

    if (marked) {
        DebugTrace(L"[DeviceManager] User action cascade tracking started: target={0} windowSeconds={1}",
                   std::wstring(deviceId),
                   c_userActionCascadeWindow.count());
    }
}

bool DeviceManager::ConsumeUserActionCascadeLocked(winrt::hstring const& deviceId) {
    PruneUserActionCascadeLocked(std::chrono::steady_clock::now());

    auto iter = m_userActionCascadeIds.find(std::wstring(deviceId));
    if (iter == m_userActionCascadeIds.end()) return false;

    m_userActionCascadeIds.erase(iter);
    return true;
}

void DeviceManager::PruneUserActionCascadeLocked(std::chrono::steady_clock::time_point now) {
    for (auto iter = m_userActionCascadeIds.begin(); iter != m_userActionCascadeIds.end();) {
        if (iter->second <= now) {
            iter = m_userActionCascadeIds.erase(iter);
        } else {
            ++iter;
        }
    }
}

void DeviceManager::EnsureDiscoveryEventHandlers() {
    if (m_discoveryDeviceAddedToken && m_discoveryDeviceRemovedToken && m_discoveryInventoryChangedToken) return;

    auto weak = weak_from_this();
    if (!m_discoveryDeviceAddedToken) {
        m_discoveryDeviceAddedToken = m_discoveryService->DeviceAdded += [weak](auto device) {
            if (auto self = weak.lock()) {
                self->OnDeviceAdded(device);
            }
        };
    }
    if (!m_discoveryDeviceRemovedToken) {
        m_discoveryDeviceRemovedToken = m_discoveryService->DeviceRemoved += [weak](auto device) {
            if (auto self = weak.lock()) {
                self->OnDeviceRemoved(device);
            }
        };
    }
    if (!m_discoveryInventoryChangedToken) {
        m_discoveryInventoryChangedToken = m_discoveryService->InventoryChanged += [weak]() {
            if (auto self = weak.lock()) {
                {
                    auto guard = self->m_lock.lock_shared();
                    if (self->m_shutdownForProcessExit) return;
                }
                self->DeviceInventoryChanged();
            }
        };
    }
}

void DeviceManager::OnConnectionStateChanged(winrt::hstring deviceId,
                                             winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
                                             winrt::Windows::Foundation::IInspectable) {
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }

    winrt::Windows::Media::Audio::AudioPlaybackConnectionState state;
    try {
        state = sender.State();
        DebugTrace(L"[DeviceManager] StateChanged: id={0} state={1}",
                   std::wstring(deviceId),
                   DeviceConnectionStateName(state));
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[DeviceManager] StateChanged callback failed: 0x{0:X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
        return;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DeviceManager] StateChanged callback failed", ex);
        return;
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] StateChanged callback failed");
        return;
    }

    if (state == winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Opened) {
        bool becameOpen = false;
        {
            auto guard = m_lock.lock_exclusive();
            if (m_sessions.IsDisconnecting(deviceId) || m_sessions.IsReconnecting(deviceId)) return;

            auto info = m_sessions.FindConnection(deviceId);
            if (!info || !info->Connection || info->Connection != sender) return;
            becameOpen = !info->IsOpen;
            if (becameOpen) {
                m_sessions.UpdateConnectionIsOpen(deviceId, true);
                m_reconnectController.CompleteConnectionSucceeded(deviceId);
                m_userActionCascadeIds.erase(std::wstring(deviceId));
            }
        }

        if (becameOpen) {
            DebugTrace(L"[DeviceManager] Incoming connection opened: {0}", std::wstring(deviceId));
            DeviceConnected(deviceId);
            DeviceStatusChanged(
                deviceId,
                winrt::hstring(_("Connected")),
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton,
                DeviceStatusKind::Connected);
            DeviceActivityChanged(deviceId);
            LogConnectionSnapshot(L"incoming-opened");
            StartConnectionHeartbeat();
        }
        return;
    }

    if (state != winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed) return;

    bool isUserActionCascade = false;
    bool retainIncomingConnection = false;
    bool reconnectOnConnectionLoss = false;
    bool wasOpen = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_sessions.IsDisconnecting(deviceId)) {
            return;
        }
        if (m_sessions.IsReconnecting(deviceId)) {
            return; // reconnect in progress – ignore stale Closed events
        }

        auto info = m_sessions.FindConnection(deviceId);
        if (!info) {
            return;
        }

        // Ignore stale callbacks from an older connection object that was already replaced.
        if (!info->Connection || info->Connection != sender) {
            return;
        }
        wasOpen = info->IsOpen;
        retainIncomingConnection = info->AcceptIncomingConnections;
        reconnectOnConnectionLoss = info->ReconnectOnConnectionLoss;
        if (retainIncomingConnection && wasOpen) {
            m_sessions.UpdateConnectionIsOpen(deviceId, false);
        }
        isUserActionCascade = ConsumeUserActionCascadeLocked(deviceId);
    }

    if (isUserActionCascade) {
        DebugTrace(L"[DeviceManager] StateChanged Closed treated as user-action cascade: {0}", std::wstring(deviceId));
        Disconnect(deviceId, DisconnectReason::UserInitiatedCascade);
        return;
    }

    if (retainIncomingConnection) {
        if (wasOpen) {
            DebugTrace(L"[DeviceManager] Incoming connection closed; sink remains enabled: {0}",
                       std::wstring(deviceId));
            DeviceDisconnected(deviceId);
        }
        DeviceStatusChanged(deviceId,
                            winrt::hstring(_("ReadyForConnection")),
                            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                            DeviceStatusKind::Ready);
        DeviceActivityChanged(deviceId);
        if (!HasConnections()) {
            StopConnectionHeartbeat();
        }
        if (wasOpen && reconnectOnConnectionLoss) {
            ScheduleReconnect(deviceId);
        }
        LogConnectionSnapshot(L"incoming-closed-ready");
        return;
    }

    Disconnect(deviceId, DisconnectReason::Unexpected);
}

void DeviceManager::ScheduleReconnect(winrt::hstring deviceId) {
    ReconnectController::TimerToken timerToken;
    try {
        ReconnectController::ScheduleDecision decision;
        {
            auto guard = m_lock.lock_exclusive();
            decision =
                m_reconnectController.PrepareSchedule(deviceId, m_shutdownForProcessExit || m_powerTransitionSuspended);
        }

        if (decision.NotifyFailed) {
            NotifyAutoReconnectFailed(deviceId, decision.MaxAttempts);
            return;
        }
        if (!decision.ShouldSchedule) return;
        timerToken = decision.Token;

        DebugTrace(L"[DeviceManager] Auto-reconnect scheduled: id={0} attempt={1} delaySeconds={2}",
                   std::wstring(deviceId),
                   decision.Attempt,
                   decision.Delay.count());
        AutoReconnectTriggered(deviceId);
        auto weak = weak_from_this();
        (void)winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [weak, timerToken](auto) {
                if (auto self = weak.lock()) {
                    self->AutoReconnectAttemptDetached(timerToken);
                }
            },
            decision.Delay);
    } catch (winrt::hresult_error const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(timerToken);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (std::exception const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(timerToken);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (...) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(timerToken);
        util::DebugTraceUnknownException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer");
    }
}

void DeviceManager::AutoReconnectAttemptDetached(ReconnectController::TimerToken token) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, ReconnectController::TimerToken timerToken) -> winrt::fire_and_forget {
        auto self = weak.lock();
        if (!self) co_return;

        const auto deviceId = winrt::hstring(timerToken.DeviceId);
        OperationToken operation;
        {
            auto guard = self->m_lock.lock_exclusive();
            auto info = self->m_sessions.FindConnection(deviceId);
            const bool blocked = self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                                 (info && info->IsOpen) || self->m_sessions.IsConnecting(deviceId) ||
                                 self->m_sessions.IsReconnecting(deviceId);
            if (!self->m_reconnectController.ClaimTimer(timerToken, blocked)) co_return;
            operation = self->BeginOperationLocked(deviceId, ConnectionIntent::AutoReconnect);
        }

        self->DeviceStatusChanged(
            deviceId,
            winrt::hstring(_("Reconnecting")),
            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton,
            DeviceStatusKind::Reconnecting);
        self->DeviceActivityChanged(deviceId);

        bool success = false;
        try {
            success = co_await self->ConnectWithIntentAsync(deviceId, operation);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] Auto reconnect attempt ignored exception");
        }

        if (!success) {
            std::shared_ptr<CloseBarrier> closeBarrier;
            {
                auto guard = self->m_lock.lock_shared();
                auto iter = self->m_closeBarriers.find(timerToken.DeviceId);
                if (iter != self->m_closeBarriers.end()) closeBarrier = iter->second;
            }
            if (closeBarrier) {
                try {
                    (void)co_await winrt::resume_on_signal(closeBarrier->Completed.get());
                } catch (...) {
                    util::DebugTraceUnknownException(L"[DeviceManager] Auto reconnect close wait ignored exception");
                }
            }
        }

        ReconnectController::ScheduleDecision completion;
        {
            auto guard = self->m_lock.lock_exclusive();
            if (success) {
                self->m_reconnectController.CompleteAttemptSucceeded(timerToken);
            } else {
                completion = self->m_reconnectController.CompleteAttemptFailed(timerToken);
            }
        }
        self->DeviceActivityChanged(deviceId);

        if (success) co_return;
        if (!completion.AttemptCompleted) co_return;
        if (completion.NotifyFailed) {
            self->NotifyAutoReconnectFailed(deviceId, completion.MaxAttempts);
            co_return;
        }
        self->ScheduleReconnect(deviceId);
    }(std::move(weak), std::move(token));
}

void DeviceManager::NotifyAutoReconnectFailed(winrt::hstring const& deviceId, std::size_t maxAttempts) {
    DebugTrace(
        L"[DeviceManager] Auto-reconnect stopped after {0} attempts for {1}", maxAttempts, std::wstring(deviceId));
    ConnectionError(deviceId, winrt::hstring(_("AutoReconnectFailed")));
    DeviceStatusChanged(deviceId,
                        winrt::hstring(_("AutoReconnectFailed")),
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                        DeviceStatusKind::Error);
    AutoReconnectFailed(deviceId);
}

void DeviceManager::OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceInformation args) {
    bool enableIncoming = false;
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
        enableIncoming = m_incomingConnectionsEnabled;
    }

    if (enableIncoming) {
        EnableIncomingConnectionDetached(std::move(args));
    }
}

void DeviceManager::OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args) {
    std::optional<bool> removedSessionWasOpen;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;

        auto info = m_sessions.FindConnection(args.Id());
        if (info && !m_sessions.IsDisconnecting(args.Id()) && !m_sessions.IsReconnecting(args.Id())) {
            removedSessionWasOpen = info->IsOpen;
        }
    }
    if (removedSessionWasOpen) {
        Disconnect(args.Id(), *removedSessionWasOpen ? DisconnectReason::Unexpected : DisconnectReason::Cleanup);
    }
}
