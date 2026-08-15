#include <pch.h>
#include <core/DeviceManager.hpp>
#include <core/AudioConnectionService.hpp>
#include <core/DeviceManagerDiagnostics.hpp>
#include <core/StringResources.hpp>
#include <thread>
#include <utility>

namespace {
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

    try {
        auto sharedConnections = std::make_shared<std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>>(
            std::move(connections));
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
                    util::DebugTraceUnknownException(
                        L"[DeviceManager] Connection cleanup completion ignored exception");
                }
            }
        };

        try {
            (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
                [sharedConnections, closeConnections](winrt::Windows::Foundation::IAsyncAction) noexcept {
                    closeConnections(sharedConnections);
                });
            return;
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] thread-pool connection cleanup scheduling failed");
        }

        try {
            std::thread([sharedConnections, closeConnections]() noexcept {
                closeConnections(sharedConnections);
            }).detach();
            return;
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] thread connection cleanup fallback failed");
        }

        closeConnections(std::move(sharedConnections));
    } catch (...) {
        for (auto& connection : connections) {
            AudioConnectionService::Close(connection);
        }
        if (completed) {
            try {
                completed();
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] Connection cleanup completion ignored exception");
            }
        }
        util::DebugTraceUnknownException(L"[DeviceManager] synchronous connection cleanup fallback used");
    }
    (void)context;
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
    std::scoped_lock lifecycleLock(m_discoveryLifecycleMutex);
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        m_reconnectController.AllowReconnects();
    }
    EnsureDiscoveryEventHandlers();
    m_discoveryService->Start();
}

void DeviceManager::StopDeviceWatcher() {
    std::scoped_lock lifecycleLock(m_discoveryLifecycleMutex);
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
            m_deviceOperations.CancelAll();
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
    struct ConnectionForSuspend {
        winrt::hstring DeviceId;
        winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
        winrt::event_token StateChangedToken{};
        std::shared_ptr<CloseBarrier> Barrier;
    };
    static_assert(std::is_nothrow_move_constructible_v<ConnectionForSuspend>);
    std::vector<ConnectionForSuspend> connections;
    decltype(m_sessions.ExtractAllConnections()) extractedConnections;
    try {
        DebugTrace(L"[DeviceManager] Power transition suspend started");
        StopDeviceWatcher();
        CancelPendingReconnects();

        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) return;
            m_powerTransitionSuspended = true;
            for (auto& entry : m_connectAttemptIds) {
                ++entry.second;
            }
            m_deviceOperations.CancelAll();
            extractedConnections = m_sessions.ExtractAllConnections();
            connections.reserve(extractedConnections.size());

            for (auto& [id, info] : extractedConnections) {
                if (info.Connection) {
                    auto deviceId = winrt::hstring(id);
                    connections.push_back({deviceId, std::move(info.Connection), info.StateChangedToken, nullptr});
                    auto& item = connections.back();
                    item.Barrier = InstallCloseBarrierLocked(deviceId);
                }
            }

            m_reconnectController.ClearTracking();
            m_userActionCascadeIds.clear();
        }

        for (auto& item : connections) {
            bool cleanupHandedOff = false;
            auto cleanupFallback = wil::scope_exit([&]() noexcept {
                if (!cleanupHandedOff) {
                    StartCloseBarrierCleanup(std::move(item.Connection),
                                             std::move(item.DeviceId),
                                             std::move(item.Barrier),
                                             false,
                                             L"Power transition cleanup fallback");
                }
            });
            if (item.StateChangedToken.value != 0) {
                AudioConnectionService::RevokeStateChanged(item.Connection, item.StateChangedToken);
            }
            StartCloseBarrierCleanup(std::move(item.Connection),
                                     std::move(item.DeviceId),
                                     std::move(item.Barrier),
                                     false,
                                     L"Power transition cleanup");
            cleanupHandedOff = true;
        }

        LogConnectionSnapshot(L"power-suspend");
    } catch (...) {
        for (auto& item : connections) {
            AudioConnectionService::RevokeStateChanged(item.Connection, item.StateChangedToken);
            StartCloseBarrierCleanup(std::move(item.Connection),
                                     std::move(item.DeviceId),
                                     std::move(item.Barrier),
                                     false,
                                     L"Power transition outer fallback");
        }
        for (auto& [id, info] : extractedConnections) {
            (void)id;
            AudioConnectionService::RevokeStateChanged(info.Connection, info.StateChangedToken);
            AudioConnectionService::Close(info.Connection);
        }
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
        if (deviceId.empty()) co_return;
        if (m_shutdownForProcessExit) {
            DebugTrace(L"[DeviceManager] ConnectAsync ignored during process exit: {0}", std::wstring(deviceId));
            co_return;
        }
        if (m_powerTransitionSuspended) {
            DebugTrace(L"[DeviceManager] ConnectAsync ignored during power transition suspend: {0}",
                       std::wstring(deviceId));
            co_return;
        }
        auto const connection = m_sessions.FindConnection(deviceId);
        if (connection && connection->IsOpen) co_return;
        auto const phase = connection && connection->AcceptIncomingConnections ? OperationPhase::Reconnecting
                                                                               : OperationPhase::Connecting;
        auto reservation = TryBeginOperationLocked(deviceId, ConnectionIntent::ManualConnect, phase);
        if (!reservation) {
            DebugTrace(L"[DeviceManager] ConnectAsync coalesced with active operation: {0}", std::wstring(deviceId));
            co_return;
        }
        operation = std::move(*reservation);
        try {
            m_reconnectController.AllowReconnects();
            m_reconnectController.BeginManualOperation(deviceId);
        } catch (...) {
            static_cast<void>(m_deviceOperations.Complete(operation));
            throw;
        }
    }

    auto completeOperation = wil::scope_exit([this, &operation]() noexcept { CompleteDeviceOperation(operation); });
    DeviceActivityChanged(deviceId);
    (void)co_await ConnectWithIntentAsync(std::move(deviceId), operation);
}

winrt::Windows::Foundation::IAsyncOperation<bool>
DeviceManager::ConnectWithIntentAsync(winrt::hstring deviceId,
                                      // The coroutine frame must own the token across suspension points.
                                      // cppcheck-suppress passedByValue
                                      OperationToken operation) {
    auto lifetime = shared_from_this();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    const bool reportFailures = operation.OperationIntent != ConnectionIntent::AutoReconnect;
    try {
        bool openEnabledIncomingConnection = false;
        bool phaseChanged = false;
        {
            auto guard = m_lock.lock_exclusive();
            if (!IsOperationCurrentLocked(operation)) co_return false;
            if (auto info = m_sessions.FindConnection(deviceId)) {
                if (!info->IsOpen && info->AcceptIncomingConnections &&
                    operation.OperationIntent != ConnectionIntent::IncomingEnable) {
                    openEnabledIncomingConnection = true;
                } else {
                    co_return info->IsOpen;
                }
            }
            if (!openEnabledIncomingConnection && m_sessions.IsDisconnecting(deviceId) &&
                !m_closeBarriers.contains(deviceIdKey)) {
                co_return false;
            }
            if (openEnabledIncomingConnection &&
                !m_deviceOperations.IsInPhase(deviceId, OperationPhase::Reconnecting)) {
                phaseChanged =
                    m_deviceOperations.Transition(operation, OperationPhase::Connecting, OperationPhase::Reconnecting);
                if (!phaseChanged) co_return false;
            }
        }
        if (phaseChanged) DeviceActivityChanged(deviceId);

        if (openEnabledIncomingConnection) {
            DebugTrace(L"[DeviceManager] ConnectAsync opening enabled incoming connection: {0}",
                       std::wstring(deviceId));
            co_return co_await ReconnectWithIntentAsync(deviceId, operation);
        }
        DebugTrace(L"[DeviceManager] ConnectAsync requested: {0}", std::wstring(deviceId));

        if (!(co_await WaitForCloseBarrierAsync(operation,
                                                operation.OperationIntent == ConnectionIntent::AutoReconnect))) {
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
            m_sessions.HasConnection(deviceId) || m_sessions.IsDisconnecting(deviceId) ||
            m_closeBarriers.contains(std::wstring(deviceId))) {
            co_return;
        }
        auto reservation =
            TryBeginOperationLocked(deviceId, ConnectionIntent::IncomingEnable, OperationPhase::EnablingIncoming);
        if (!reservation) co_return;
        operation = std::move(*reservation);
    }

    auto completeOperation = wil::scope_exit([this, &operation]() noexcept { CompleteDeviceOperation(operation); });
    DeviceActivityChanged(deviceId);
    (void)co_await ConnectInternalAsync(device, false, operation, true);
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
            auto inventory = self->m_discoveryService->GetInventorySnapshot();
            for (auto const& identity : inventory.Devices) {
                {
                    auto guard = self->m_lock.lock_shared();
                    if (self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                        !self->m_incomingConnectionsEnabled) {
                        co_return;
                    }
                }
                auto device = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(
                    winrt::hstring(identity.Id));
                if (device) co_await self->EnableIncomingConnectionAsync(std::move(device));
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
                    self->m_deviceOperations.IsActive(std::wstring_view(id)) ||
                    self->m_closeBarriers.contains(std::wstring(id))) {
                    co_return;
                }
            }

            auto device =
                co_await winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(std::move(id));
            if (device) co_await self->EnableIncomingConnectionAsync(std::move(device));
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
        if (deviceId.empty()) co_return;
        if (m_shutdownForProcessExit) {
            DebugTrace(L"[DeviceManager] ReconnectAsync ignored during process exit: {0}", std::wstring(deviceId));
            co_return;
        }
        if (m_powerTransitionSuspended) {
            DebugTrace(L"[DeviceManager] ReconnectAsync ignored during power transition suspend: {0}",
                       std::wstring(deviceId));
            co_return;
        }
        auto reservation =
            TryBeginOperationLocked(deviceId, ConnectionIntent::ManualReconnect, OperationPhase::Reconnecting);
        if (!reservation) {
            DebugTrace(L"[DeviceManager] ReconnectAsync coalesced with active operation: {0}", std::wstring(deviceId));
            co_return;
        }
        operation = std::move(*reservation);
        try {
            m_reconnectController.AllowReconnects();
            m_reconnectController.BeginManualOperation(deviceId);
        } catch (...) {
            static_cast<void>(m_deviceOperations.Complete(operation));
            throw;
        }
    }

    auto completeOperation = wil::scope_exit([this, &operation]() noexcept { CompleteDeviceOperation(operation); });
    DeviceActivityChanged(deviceId);
    (void)co_await ReconnectWithIntentAsync(std::move(deviceId), operation);
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
            if (!m_deviceOperations.IsInPhase(deviceId, OperationPhase::Reconnecting)) co_return false;
        }

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
        try {
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
        } catch (...) {
            AudioConnectionService::RevokeStateChanged(oldConn, oldToken);
            AudioConnectionService::Close(oldConn);
            if (closeBarrier) {
                CompleteCloseBarrierDetached(deviceId, std::move(closeBarrier), false);
            }
            throw;
        }

        if (closeBarrier) {
            bool cleanupHandedOff = false;
            auto cleanupFallback = wil::scope_exit([&]() noexcept {
                if (!cleanupHandedOff) {
                    StartCloseBarrierCleanup(
                        std::move(oldConn), deviceId, std::move(closeBarrier), false, L"Reconnect cleanup fallback");
                }
            });
            // Revoke the event token first so no stale Closed callbacks fire.
            if (oldToken.value != 0) {
                AudioConnectionService::RevokeStateChanged(oldConn, oldToken);
            }
            DebugTrace(L"[DeviceManager] ReconnectAsync closing old connection: {0}", std::wstring(deviceId));
            StartCloseBarrierCleanup(
                std::move(oldConn), deviceId, std::move(closeBarrier), false, L"Reconnect cleanup");
            cleanupHandedOff = true;
        }

        if (!(co_await WaitForCloseBarrierAsync(operation,
                                                operation.OperationIntent == ConnectionIntent::AutoReconnect))) {
            co_return false;
        }
        if (!IsOperationCurrent(operation)) co_return false;
        co_return co_await ConnectWithIntentAsync(deviceId, operation);
    } catch (winrt::hresult_error const& ex) {
        if (operation.OperationIntent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
            ReportAsyncConnectionError(*this, deviceId, ex.message(), L"ReconnectAsync");
        }
    } catch (std::exception const& ex) {
        if (operation.OperationIntent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
            ReportAsyncConnectionError(
                *this, deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), L"ReconnectAsync");
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ReconnectAsync ERROR");
        if (operation.OperationIntent != ConnectionIntent::AutoReconnect && IsOperationCurrent(operation)) {
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
        for (auto const& operation : m_deviceOperations.Snapshot()) {
            affectedIds.insert(operation.DeviceId);
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
            if (info.IsOpen && !m_sessions.IsDisconnecting(winrt::hstring(id)) && !m_deviceOperations.IsActive(id)) {
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
    bool activityChanged = false;
    std::optional<ReconnectPolicyCleanup> cleanup;
    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.SetReconnectOnConnectionLoss(deviceId, enabled);
        cancelActiveAttempt = !enabled && m_reconnectController.HasAttemptInProgress(deviceId);
        activityChanged = !enabled && m_reconnectController.HasPendingTimer(deviceId) && !cancelActiveAttempt;
        m_reconnectController.SetPolicyEnabled(deviceId, enabled);
        if (cancelActiveAttempt) cleanup = PrepareReconnectPolicyCleanupLocked(deviceId);
    }
    if (cleanup) StartReconnectPolicyCleanup(std::move(*cleanup));
    if (activityChanged) DeviceActivityChanged(deviceId);
}

void DeviceManager::ApplyReconnectOnConnectionLossPolicy(bool globallyEnabled,
                                                         std::span<const std::wstring> individuallyEnabledDeviceIds) {
    std::unordered_set<std::wstring> individuallyEnabled(individuallyEnabledDeviceIds.begin(),
                                                         individuallyEnabledDeviceIds.end());
    std::vector<ReconnectPolicyCleanup> cleanups;
    std::vector<std::wstring> activityChanges;
    static_assert(std::is_nothrow_move_constructible_v<ReconnectPolicyCleanup>);
    try {
        auto guard = m_lock.lock_exclusive();
        std::unordered_set<std::wstring> trackedIds;
        for (auto const& [id, info] : m_sessions.GetConnectionsSnapshot()) {
            (void)info;
            trackedIds.insert(id);
        }
        for (auto& id : m_reconnectController.PendingDeviceIds()) {
            trackedIds.insert(std::move(id));
        }
        cleanups.reserve(trackedIds.size());
        activityChanges.reserve(trackedIds.size());
        for (auto const& id : trackedIds) {
            auto const enabled = globallyEnabled || individuallyEnabled.contains(id);
            auto const pending = m_reconnectController.HasPendingTimer(id);
            auto const cancelAttempt = !enabled && m_reconnectController.HasAttemptInProgress(id);
            m_sessions.SetReconnectOnConnectionLoss(winrt::hstring(id), enabled);
            m_reconnectController.SetPolicyEnabled(id, enabled);
            if (pending && !enabled && !cancelAttempt) activityChanges.push_back(id);
            if (cancelAttempt) {
                auto cleanup = PrepareReconnectPolicyCleanupLocked(winrt::hstring(id));
                if (cleanup) cleanups.push_back(std::move(*cleanup));
            }
        }
    } catch (...) {
        for (auto& cleanup : cleanups) {
            StartReconnectPolicyCleanup(std::move(cleanup));
        }
        throw;
    }

    for (auto& cleanup : cleanups) {
        StartReconnectPolicyCleanup(std::move(cleanup));
    }
    for (auto const& id : activityChanges) {
        try {
            DeviceActivityChanged(winrt::hstring(id));
        } catch (...) {
            util::DebugTraceUnknownException(
                L"[DeviceManager] reconnect policy activity notification ignored exception");
        }
    }
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
    return m_deviceOperations.HasActiveOperations() || !m_closeBarriers.empty() ||
           m_reconnectController.HasPendingTimers();
}

bool DeviceManager::IsDeviceBusy(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    auto const key = std::wstring(deviceId);
    return m_deviceOperations.IsActive(key) || m_closeBarriers.contains(key) ||
           m_reconnectController.HasPendingTimer(deviceId);
}

apc::device_picker::DeviceActivitySnapshot DeviceManager::GetDevicePickerActivitySnapshot() const {
    auto guard = m_lock.lock_shared();
    auto result = m_sessions.GetDevicePickerActivitySnapshot();
    for (auto const& operation : m_deviceOperations.Snapshot()) {
        result.BusyIds.insert(operation.DeviceId);
    }
    for (auto& id : m_reconnectController.PendingDeviceIds()) {
        result.BusyIds.insert(std::move(id));
    }
    return result;
}

apc::device_picker::DeviceInventorySnapshot DeviceManager::GetDevicePickerInventorySnapshot() const {
    auto discoveryService = m_discoveryService;
    return discoveryService ? discoveryService->GetInventorySnapshot() : apc::device_picker::DeviceInventorySnapshot{};
}

std::optional<apc::device_picker::DeviceInventorySnapshot>
DeviceManager::GetDevicePickerInventorySnapshotIfChanged(std::uint64_t knownGeneration) const {
    auto discoveryService = m_discoveryService;
    if (!discoveryService) return std::nullopt;
    return discoveryService->GetInventorySnapshotIfChanged(knownGeneration);
}

DeviceTrayPresentationSnapshot DeviceManager::GetTrayPresentationSnapshot() const {
    auto guard = m_lock.lock_shared();
    DeviceTrayPresentationSnapshot snapshot;
    snapshot.ConnectedDevices = m_sessions.ConnectedDevicePresentations();
    snapshot.HasBusyOperations = m_deviceOperations.HasActiveOperations() || !m_closeBarriers.empty() ||
                                 m_reconnectController.HasPendingTimers();
    return snapshot;
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
            snapshot.Reconnecting = m_deviceOperations.IsInPhase(id, OperationPhase::Reconnecting);
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
    OperationToken const* expectedOperation = nullptr;
    static_cast<void>(
        DisconnectInternal(std::move(deviceId), reason, suppressCascade, expectedOperation, 0, nullptr, false));
}

bool DeviceManager::DisconnectIfCurrent(winrt::hstring deviceId,
                                        DisconnectReason reason,
                                        bool suppressCascade,
                                        OperationToken const& expectedOperation,
                                        std::size_t expectedAttemptId) {
    return DisconnectInternal(
        std::move(deviceId), reason, suppressCascade, &expectedOperation, expectedAttemptId, nullptr, false);
}

bool DeviceManager::DisconnectIfCurrentConnection(
    winrt::hstring deviceId,
    DisconnectReason reason,
    winrt::Windows::Media::Audio::AudioPlaybackConnection const& expectedConnection) {
    return DisconnectInternal(std::move(deviceId), reason, false, nullptr, 0, nullptr, false, expectedConnection, true);
}

bool DeviceManager::DisconnectInternal(winrt::hstring deviceId,
                                       DisconnectReason reason,
                                       bool suppressCascade,
                                       OperationToken const* expectedOperation,
                                       std::size_t expectedAttemptId,
                                       winrt::hstring const* failureMessage,
                                       bool restoreIncomingIfNoConnection,
                                       winrt::Windows::Media::Audio::AudioPlaybackConnection const& expectedConnection,
                                       bool deriveDeviceRemovalReason) {
    auto reasonName = [](DisconnectReason value) -> std::wstring_view {
        switch (value) {
            case DisconnectReason::UserInitiated: return L"UserInitiated";
            case DisconnectReason::UserInitiatedCascade: return L"UserInitiatedCascade";
            case DisconnectReason::Unexpected: return L"Unexpected";
            case DisconnectReason::Cleanup: return L"Cleanup";
            default: return L"UnknownReason";
        }
    };
    const std::wstring deviceIdKey(deviceId);

    bool reconnectOnConnectionLoss = false;
    bool acceptIncomingConnections = false;
    bool powerTransitionSuspended = false;
    bool restoreIncoming = false;
    bool stateChanged = false;
    winrt::Windows::Media::Audio::AudioPlaybackConnection connection{nullptr};
    winrt::event_token stateChangedToken{};
    std::shared_ptr<CloseBarrier> closeBarrier;
    try {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return false;

        if (expectedOperation) {
            auto const attempt = m_connectAttemptIds.find(deviceIdKey);
            if (!IsOperationCurrentLocked(*expectedOperation) || attempt == m_connectAttemptIds.end() ||
                attempt->second != expectedAttemptId) {
                return false;
            }
        }
        if (expectedConnection) {
            auto const current = m_sessions.FindConnection(deviceId);
            if (!current || current->Connection != expectedConnection) return false;
            if (deriveDeviceRemovalReason) {
                if (m_sessions.IsDisconnecting(deviceId) ||
                    m_deviceOperations.IsInPhase(deviceIdKey, OperationPhase::Reconnecting)) {
                    return false;
                }
                reason = current->IsOpen ? DisconnectReason::Unexpected : DisconnectReason::Cleanup;
            }
        }

        InvalidateDeviceOperationLocked(deviceId);
        ++m_connectAttemptIds[deviceIdKey];
        if (reason == DisconnectReason::UserInitiated) {
            m_reconnectController.CancelDevice(deviceId);
        }

        auto extracted = m_sessions.ExtractConnection(deviceId);
        if (!extracted) {
            stateChanged = true;
            if (expectedOperation) {
                restoreIncoming =
                    restoreIncomingIfNoConnection && m_incomingConnectionsEnabled && !m_powerTransitionSuspended;
                closeBarrier = InstallCloseBarrierLocked(deviceId);
            }
        } else {
            connection = std::move(extracted->Connection);
            stateChangedToken = extracted->StateChangedToken;
            reconnectOnConnectionLoss = extracted->ReconnectOnConnectionLoss;
            acceptIncomingConnections = extracted->AcceptIncomingConnections;
            if (m_powerTransitionSuspended) {
                reconnectOnConnectionLoss = false;
                acceptIncomingConnections = false;
            }
            powerTransitionSuspended = m_powerTransitionSuspended;
            restoreIncoming = acceptIncomingConnections && m_incomingConnectionsEnabled &&
                              (reason == DisconnectReason::UserInitiated || reason == DisconnectReason::Cleanup);
            closeBarrier = InstallCloseBarrierLocked(deviceId);
            stateChanged = true;
            if (reason == DisconnectReason::UserInitiated && !suppressCascade) {
                TrackUserActionCascadeLocked(deviceId);
            }
        }
    } catch (...) {
        AudioConnectionService::RevokeStateChanged(connection, stateChangedToken);
        AudioConnectionService::Close(connection);
        if (closeBarrier) {
            CompleteCloseBarrierDetached(deviceId, std::move(closeBarrier), restoreIncoming);
        }
        throw;
    }

    DebugTrace(L"[DeviceManager] Disconnect claimed: id={0} reason={1} suppressCascade={2}",
               deviceIdKey,
               reasonName(reason),
               suppressCascade);

    if (!closeBarrier) {
        if (stateChanged) DeviceActivityChanged(deviceId);
        return stateChanged;
    }

    bool cleanupHandedOff = false;
    auto cleanupFallback = wil::scope_exit([&]() noexcept {
        if (!cleanupHandedOff) {
            StartCloseBarrierCleanup(std::move(connection),
                                     deviceId,
                                     std::move(closeBarrier),
                                     restoreIncoming,
                                     L"Disconnect cleanup fallback");
        }
    });

    // Revoke the StateChanged token so the zombie cannot fire events at us.
    if (connection && stateChangedToken.value != 0) {
        AudioConnectionService::RevokeStateChanged(connection, stateChangedToken);
    }

    if (failureMessage) {
        ConnectionError(deviceId, *failureMessage);
        DeviceStatusChanged(deviceId,
                            *failureMessage,
                            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                            DeviceStatusKind::Error);
    }

    bool activityPublished = false;
    if (reason != DisconnectReason::Cleanup && !powerTransitionSuspended) {
        if (reason == DisconnectReason::UserInitiatedCascade) {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            if (reconnectOnConnectionLoss) {
                activityPublished = ScheduleReconnect(deviceId);
            }
        } else {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            if (reason == DisconnectReason::Unexpected && reconnectOnConnectionLoss) {
                activityPublished = ScheduleReconnect(deviceId);
            }
        }
    }

    if (!activityPublished) DeviceActivityChanged(deviceId);

    StartCloseBarrierCleanup(
        std::move(connection), deviceId, std::move(closeBarrier), restoreIncoming, L"Disconnect cleanup");
    cleanupHandedOff = true;

    LogConnectionSnapshot(winrt::hstring(L"disconnect:") + winrt::hstring(reasonName(reason)));
    return true;
}

void DeviceManager::ReportConnectionFailure(winrt::hstring const& deviceId,
                                            winrt::hstring const& message,
                                            OperationToken const& operation,
                                            std::size_t attemptId,
                                            bool restoreIncomingIfNoConnection) {
    static_cast<void>(DisconnectInternal(
        deviceId, DisconnectReason::Cleanup, false, &operation, attemptId, &message, restoreIncomingIfNoConnection));
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
                        ReportConnectionFailure(
                            deviceId, winrt::hstring(_("UnknownError")), operation, attemptId, openImmediately);
                    } else {
                        static_cast<void>(DisconnectInternal(deviceId,
                                                             DisconnectReason::Cleanup,
                                                             false,
                                                             &operation,
                                                             attemptId,
                                                             nullptr,
                                                             openImmediately));
                    }
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
                    m_reconnectController.SetPolicyEnabled(deviceId, info.ReconnectOnConnectionLoss);
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

            if (!IsOperationCurrent(operation)) co_return false;

            if (openImmediately && operation.OperationIntent == ConnectionIntent::ManualConnect) {
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
                    static_cast<void>(
                        DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
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
                        if (operation.OperationIntent != ConnectionIntent::AutoReconnect) {
                            m_reconnectController.CompleteConnectionSucceeded(deviceId);
                        }
                        m_userActionCascadeIds.erase(deviceIdKey);
                    }
                    if (becameOpen) {
                        DeviceConnected(deviceId);
                        DeviceStatusChanged(deviceId,
                                            winrt::hstring(_("Connected")),
                                            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::
                                                ShowDisconnectButton,
                                            DeviceStatusKind::Connected);
                    }
                    LogConnectionSnapshot(L"open-success");
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
                        ReportConnectionFailure(deviceId, winrt::hstring(_("RequestTimedOut")), operation, attemptId);
                    } else {
                        static_cast<void>(
                            DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
                    }
                    co_return false;
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
                    if (reportFailures) {
                        ReportConnectionFailure(deviceId, winrt::hstring(_("DeniedBySystem")), operation, attemptId);
                    } else {
                        static_cast<void>(
                            DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
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
                        ReportConnectionFailure(deviceId, err.message(), operation, attemptId);
                    } else {
                        static_cast<void>(
                            DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
                    }
                    co_return false;
                }
            }
        }
    } catch (winrt::hresult_error const& ex) {
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, ex.message(), operation, attemptId);
        } else {
            static_cast<void>(DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
        }
    } catch (std::exception const& ex) {
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), operation, attemptId);
        } else {
            static_cast<void>(DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ConnectInternalAsync ERROR");
        if (!IsConnectAttemptCurrent(operation, attemptId)) co_return false;
        if (reportFailures) {
            ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), operation, attemptId);
        } else {
            static_cast<void>(DisconnectIfCurrent(deviceId, DisconnectReason::Cleanup, false, operation, attemptId));
        }
    }
    co_return false;
}

std::optional<DeviceManager::OperationToken>
DeviceManager::TryBeginOperationLocked(winrt::hstring const& deviceId, ConnectionIntent intent, OperationPhase phase) {
    return m_deviceOperations.TryBegin(std::wstring_view(deviceId), intent, phase);
}

void DeviceManager::InvalidateDeviceOperationLocked(winrt::hstring const& deviceId) {
    static_cast<void>(m_deviceOperations.Invalidate(std::wstring_view(deviceId)));
}

std::optional<DeviceManager::ReconnectPolicyCleanup>
DeviceManager::PrepareReconnectPolicyCleanupLocked(winrt::hstring const& deviceId) {
    auto attemptId = m_connectAttemptIds.try_emplace(std::wstring(deviceId), 0).first;
    InvalidateDeviceOperationLocked(deviceId);
    ++attemptId->second;

    if (!m_sessions.HasConnection(deviceId)) return std::nullopt;
    ReconnectPolicyCleanup cleanup;
    cleanup.DeviceId = deviceId;
    auto barrier = InstallCloseBarrierLocked(deviceId);
    std::optional<DeviceConnectionInfo> extracted;
    try {
        extracted = m_sessions.ExtractConnection(deviceId);
    } catch (...) {
        static_cast<void>(RemoveCloseBarrierLocked(barrier));
        throw;
    }
    if (!extracted) {
        static_cast<void>(RemoveCloseBarrierLocked(barrier));
        return std::nullopt;
    }

    cleanup.Connection = std::move(extracted->Connection);
    cleanup.StateChangedToken = extracted->StateChangedToken;
    cleanup.Barrier = std::move(barrier);
    cleanup.RestoreIncoming = extracted->AcceptIncomingConnections && m_incomingConnectionsEnabled;
    return cleanup;
}

void DeviceManager::StartReconnectPolicyCleanup(ReconnectPolicyCleanup cleanup) noexcept {
    bool cleanupHandedOff = false;
    auto cleanupFallback = wil::scope_exit([&]() noexcept {
        if (!cleanupHandedOff) {
            StartCloseBarrierCleanup(std::move(cleanup.Connection),
                                     std::move(cleanup.DeviceId),
                                     std::move(cleanup.Barrier),
                                     cleanup.RestoreIncoming,
                                     L"Reconnect policy cleanup fallback");
        }
    });
    try {
        if (cleanup.Connection && cleanup.StateChangedToken.value != 0) {
            AudioConnectionService::RevokeStateChanged(cleanup.Connection, cleanup.StateChangedToken);
        }
        StartCloseBarrierCleanup(std::move(cleanup.Connection),
                                 std::move(cleanup.DeviceId),
                                 std::move(cleanup.Barrier),
                                 cleanup.RestoreIncoming,
                                 L"Reconnect policy cleanup");
        cleanupHandedOff = true;
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] reconnect policy cleanup fallback used");
    }
}

void DeviceManager::CompleteDeviceOperation(OperationToken const& operation) noexcept {
    try {
        bool changed = false;
        {
            auto guard = m_lock.lock_exclusive();
            changed = m_deviceOperations.Complete(operation);
        }
        if (changed) DeviceActivityChanged(winrt::hstring(operation.DeviceId));
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] operation completion ignored exception");
    }
}

bool DeviceManager::IsOperationCurrent(OperationToken const& operation) const {
    auto guard = m_lock.lock_shared();
    return IsOperationCurrentLocked(operation);
}

bool DeviceManager::IsOperationCurrentLocked(OperationToken const& operation) const {
    if (m_shutdownForProcessExit || m_powerTransitionSuspended) return false;
    return m_deviceOperations.IsCurrent(operation);
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

    auto barrier = std::make_shared<CloseBarrier>();
    auto const [inserted, wasInserted] = m_closeBarriers.emplace(key, barrier);
    if (!wasInserted) return inserted->second;
    auto rollback = wil::scope_exit([&]() noexcept { m_closeBarriers.erase(inserted); });
    m_sessions.MarkDisconnecting(deviceId);
    rollback.release();
    return barrier;
}

bool DeviceManager::RemoveCloseBarrierLocked(std::shared_ptr<CloseBarrier> const& barrier) noexcept {
    auto const iter =
        std::ranges::find_if(m_closeBarriers, [&](auto const& entry) noexcept { return entry.second == barrier; });
    if (iter == m_closeBarriers.end()) return false;
    m_sessions.UnmarkDisconnecting(std::wstring_view(iter->first));
    m_closeBarriers.erase(iter);
    return true;
}

void DeviceManager::StartCloseBarrierCleanup(winrt::Windows::Media::Audio::AudioPlaybackConnection connection,
                                             winrt::hstring deviceId,
                                             std::shared_ptr<CloseBarrier> barrier,
                                             bool restoreIncoming,
                                             std::wstring_view context) noexcept {
    if (!barrier) {
        AudioConnectionService::Close(connection);
        return;
    }

    auto fallback = wil::scope_exit([&]() noexcept {
        AudioConnectionService::Close(connection);
        CompleteCloseBarrierDetached(std::move(deviceId), std::move(barrier), restoreIncoming);
    });
    try {
        if (!connection) {
            CompleteCloseBarrierDetached(std::move(deviceId), std::move(barrier), restoreIncoming);
            fallback.release();
            return;
        }

        auto weak = weak_from_this();
        std::function<void()> completed =
            [weak, id = deviceId, closeBarrier = barrier, restoreIncoming]() mutable noexcept {
                if (auto self = weak.lock()) {
                    self->CompleteCloseBarrierDetached(std::move(id), std::move(closeBarrier), restoreIncoming);
                } else if (closeBarrier) {
                    closeBarrier->Completed.SetEvent();
                }
            };
        std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> connections;
        connections.push_back(std::move(connection));
        CloseConnectionsOnBackgroundThread(std::move(connections), context, std::move(completed));
        fallback.release();
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] close-barrier cleanup handoff failed");
    }
}

void DeviceManager::FinalizeCloseBarrierNowNoThrow(winrt::hstring const& deviceId,
                                                   std::shared_ptr<CloseBarrier> const& barrier,
                                                   bool restoreIncoming) noexcept {
    bool completedCurrentBarrier = false;
    bool reenable = false;
    {
        auto guard = m_lock.lock_exclusive();
        completedCurrentBarrier = RemoveCloseBarrierLocked(barrier);
        if (completedCurrentBarrier) {
            reenable = restoreIncoming && !m_shutdownForProcessExit && !m_powerTransitionSuspended &&
                       m_incomingConnectionsEnabled;
        }
    }

    if (completedCurrentBarrier) {
        try {
            DeviceActivityChanged(deviceId);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] close-barrier activity notification failed");
        }
    }
    if (barrier) barrier->Completed.SetEvent();
    if (reenable) {
        try {
            ReenableIncomingConnectionDetached(deviceId);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] close-barrier incoming restore failed");
        }
    }
}

void DeviceManager::CompleteCloseBarrierDetached(winrt::hstring deviceId,
                                                 std::shared_ptr<CloseBarrier> barrier,
                                                 bool restoreIncoming) noexcept {
    auto fallbackDeviceId = deviceId;
    auto fallbackBarrier = barrier;
    try {
        auto weak = weak_from_this();
        [](std::weak_ptr<DeviceManager> weak,
           winrt::hstring id,
           std::shared_ptr<CloseBarrier> closeBarrier,
           bool shouldRestoreIncoming) -> winrt::fire_and_forget {
            bool cooldownCompleted = false;
            try {
                co_await winrt::resume_after(ReconnectController::ReconnectCloseCooldown());
                cooldownCompleted = true;
                if (auto self = weak.lock()) {
                    self->FinalizeCloseBarrierNowNoThrow(id, closeBarrier, shouldRestoreIncoming);
                } else if (closeBarrier) {
                    closeBarrier->Completed.SetEvent();
                }
                co_return;
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] close-barrier completion fallback used");
            }

            if (!cooldownCompleted) {
                std::this_thread::sleep_for(ReconnectController::ReconnectCloseCooldown());
            }
            if (auto self = weak.lock()) {
                self->FinalizeCloseBarrierNowNoThrow(id, closeBarrier, shouldRestoreIncoming);
            } else if (closeBarrier) {
                closeBarrier->Completed.SetEvent();
            }
        }(std::move(weak), std::move(deviceId), std::move(barrier), restoreIncoming);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] close-barrier coroutine launch failed");
        std::this_thread::sleep_for(ReconnectController::ReconnectCloseCooldown());
        FinalizeCloseBarrierNowNoThrow(fallbackDeviceId, fallbackBarrier, restoreIncoming);
    }
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
            if (m_sessions.IsDisconnecting(deviceId) ||
                m_deviceOperations.IsInPhase(std::wstring_view(deviceId), OperationPhase::Reconnecting)) {
                return;
            }

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
        if (m_deviceOperations.IsInPhase(std::wstring_view(deviceId), OperationPhase::Reconnecting)) {
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
        auto const activityPublished = wasOpen && reconnectOnConnectionLoss && ScheduleReconnect(deviceId);
        if (!activityPublished) DeviceActivityChanged(deviceId);
        LogConnectionSnapshot(L"incoming-closed-ready");
        return;
    }

    Disconnect(deviceId, DisconnectReason::Unexpected);
}

bool DeviceManager::ScheduleReconnect(winrt::hstring deviceId) {
    ReconnectController::ScheduleDecision decision;
    {
        auto guard = m_lock.lock_exclusive();
        decision =
            m_reconnectController.PrepareSchedule(deviceId, m_shutdownForProcessExit || m_powerTransitionSuspended);
    }

    if (decision.NotifyFailed) {
        NotifyAutoReconnectFailed(deviceId, decision.MaxAttempts);
        return false;
    }
    return StartReconnectTimer(decision, true);
}

bool DeviceManager::StartReconnectTimer(ReconnectController::ScheduleDecision const& decision,
                                        bool notifyTriggered,
                                        bool publishScheduledActivity) {
    if (!decision.ShouldSchedule) return false;

    auto handleCreateFailure = [&]() noexcept {
        bool activityChanged = false;
        try {
            auto guard = m_lock.lock_exclusive();
            activityChanged = m_reconnectController.HandleTimerCreateFailed(decision.Token);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] reconnect timer failure cleanup failed");
            return false;
        }
        if (activityChanged) {
            try {
                DeviceActivityChanged(winrt::hstring(decision.Token.DeviceId));
            } catch (...) {
                util::DebugTraceUnknownException(
                    L"[DeviceManager] reconnect timer failure activity notification ignored exception");
            }
        }
        return activityChanged;
    };
    try {
        auto weak = weak_from_this();
        (void)winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [weak, timerToken = decision.Token](auto) {
                if (auto self = weak.lock()) {
                    try {
                        self->AutoReconnectAttemptDetached(timerToken);
                    } catch (...) {
                        bool activityChanged = false;
                        try {
                            auto guard = self->m_lock.lock_exclusive();
                            activityChanged = self->m_reconnectController.AbortTimerOrAttempt(timerToken);
                        } catch (...) {
                            util::DebugTraceUnknownException(
                                L"[DeviceManager] reconnect launch rollback ignored exception");
                        }
                        if (activityChanged) {
                            try {
                                self->DeviceActivityChanged(winrt::hstring(timerToken.DeviceId));
                            } catch (...) {
                            }
                        }
                        util::DebugTraceUnknownException(L"[DeviceManager] failed to launch auto reconnect callback");
                    }
                }
            },
            decision.Delay);
    } catch (winrt::hresult_error const& ex) {
        auto const activityChanged = handleCreateFailure();
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
        return activityChanged;
    } catch (std::exception const& ex) {
        auto const activityChanged = handleCreateFailure();
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
        return activityChanged;
    } catch (...) {
        auto const activityChanged = handleCreateFailure();
        util::DebugTraceUnknownException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer");
        return activityChanged;
    }

    if (publishScheduledActivity) {
        try {
            DeviceActivityChanged(winrt::hstring(decision.Token.DeviceId));
        } catch (...) {
            util::DebugTraceUnknownException(
                L"[DeviceManager] reconnect timer activity notification ignored exception");
        }
    }
    try {
        DebugTrace(L"[DeviceManager] Auto-reconnect scheduled: id={0} attempt={1} delaySeconds={2}",
                   decision.Token.DeviceId,
                   decision.Attempt,
                   decision.Delay.count());
        if (notifyTriggered) AutoReconnectTriggered(winrt::hstring(decision.Token.DeviceId));
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] reconnect timer notification ignored exception");
    }
    return publishScheduledActivity;
}

void DeviceManager::AutoReconnectAttemptDetached(ReconnectController::TimerToken token) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, ReconnectController::TimerToken timerToken) -> winrt::fire_and_forget {
        auto self = weak.lock();
        if (!self) co_return;

        OperationToken operation;
        try {
            const auto deviceId = winrt::hstring(timerToken.DeviceId);
            ReconnectController::ScheduleDecision deferred;
            bool timerRetired = false;
            {
                auto guard = self->m_lock.lock_exclusive();
                auto info = self->m_sessions.FindConnection(deviceId);
                if (self->m_shutdownForProcessExit || self->m_powerTransitionSuspended || (info && info->IsOpen)) {
                    timerRetired = self->m_reconnectController.RetireTimer(timerToken);
                } else {
                    auto reservation = self->TryBeginOperationLocked(
                        deviceId, ConnectionIntent::AutoReconnect, OperationPhase::Reconnecting);
                    if (!reservation) {
                        deferred = self->m_reconnectController.DeferTimer(timerToken);
                    } else if (!self->m_reconnectController.ClaimTimer(timerToken)) {
                        static_cast<void>(self->m_deviceOperations.Complete(*reservation));
                    } else {
                        operation = std::move(*reservation);
                    }
                }
            }
            if (timerRetired) {
                self->DeviceActivityChanged(deviceId);
                co_return;
            }
            if (deferred.ShouldSchedule) {
                static_cast<void>(self->StartReconnectTimer(deferred, false, false));
                co_return;
            }
            if (operation.Id == 0) co_return;

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
                        util::DebugTraceUnknownException(
                            L"[DeviceManager] Auto reconnect close wait ignored exception");
                    }
                }
            }

            ReconnectController::ScheduleDecision completion;
            {
                auto guard = self->m_lock.lock_exclusive();
                static_cast<void>(self->m_deviceOperations.Complete(operation));
                if (success) {
                    self->m_reconnectController.CompleteAttemptSucceeded(timerToken);
                } else {
                    completion = self->m_reconnectController.CompleteAttemptFailed(timerToken);
                }
            }
            if (success) {
                self->DeviceActivityChanged(deviceId);
                co_return;
            }
            if (!completion.AttemptCompleted) {
                self->DeviceActivityChanged(deviceId);
                co_return;
            }
            if (completion.NotifyFailed) {
                self->DeviceActivityChanged(deviceId);
                self->NotifyAutoReconnectFailed(deviceId, completion.MaxAttempts);
                co_return;
            }
            if (!self->ScheduleReconnect(deviceId)) self->DeviceActivityChanged(deviceId);
        } catch (...) {
            bool activityChanged = false;
            {
                auto guard = self->m_lock.lock_exclusive();
                if (operation.Id != 0) activityChanged = self->m_deviceOperations.Complete(operation);
                activityChanged = self->m_reconnectController.AbortTimerOrAttempt(timerToken) || activityChanged;
            }
            if (activityChanged) {
                try {
                    self->DeviceActivityChanged(winrt::hstring(timerToken.DeviceId));
                } catch (...) {
                }
            }
            util::DebugTraceUnknownException(L"[DeviceManager] auto reconnect callback ignored exception");
        }
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
    winrt::Windows::Media::Audio::AudioPlaybackConnection removedConnection{nullptr};
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;

        auto info = m_sessions.FindConnection(args.Id());
        if (info && info->Connection) removedConnection = info->Connection;
    }
    if (removedConnection) {
        static_cast<void>(DisconnectIfCurrentConnection(args.Id(), DisconnectReason::Cleanup, removedConnection));
    }
}
