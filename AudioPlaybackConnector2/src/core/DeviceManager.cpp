#include <pch.h>
#include <core/DeviceManager.hpp>
#include <core/AudioConnectionService.hpp>
#include <core/StringResources.hpp>
#include <utility>

namespace {
constexpr int c_heartbeatIntervalMinutes = 5;
constexpr std::chrono::seconds c_userActionCascadeWindow{5};

std::wstring_view ConnectionStateName(winrt::Windows::Media::Audio::AudioPlaybackConnectionState state) {
    switch (state) {
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed: return L"Closed";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Opened: return L"Opened";
        default: return L"UnknownState";
    }
}

std::wstring_view OpenResultStatusName(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus status) {
    switch (status) {
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: return L"Success";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
            return L"RequestTimedOut";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
            return L"DeniedBySystem";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
            return L"UnknownFailure";
        default: return L"UnknownStatus";
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
        }

        StopDeviceWatcher();

        struct ConnectionForShutdown {
            winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
            winrt::event_token StateChangedToken{};
        };
        std::vector<ConnectionForShutdown> connections;
        {
            auto guard = m_lock.lock_exclusive();
            connections.reserve(m_sessions.Connections().size() + m_sessions.ZombieConnections().size());

            for (auto& [id, info] : m_sessions.Connections()) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : m_sessions.ZombieConnections()) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_sessions.Clear();
            m_reconnectController.ClearTracking();
            m_connectAttemptIds.clear();
            m_userActionCascadeIds.clear();
            m_autoReconnectPred = nullptr;
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
            winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
            winrt::event_token StateChangedToken{};
        };
        std::vector<ConnectionForSuspend> connections;
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) return;
            m_powerTransitionSuspended = true;
            connections.reserve(m_sessions.Connections().size() + m_sessions.ZombieConnections().size());

            for (auto& [id, info] : m_sessions.Connections()) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : m_sessions.ZombieConnections()) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_sessions.Clear();
            m_reconnectController.ClearTracking();
            m_userActionCascadeIds.clear();
        }

        if (!connections.empty()) {
            std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> closeConnections;
            closeConnections.reserve(connections.size());
            for (auto& item : connections) {
                if (item.Connection && item.StateChangedToken.value != 0) {
                    AudioConnectionService::RevokeStateChanged(item.Connection, item.StateChangedToken);
                }
                if (item.Connection) {
                    closeConnections.push_back(std::move(item.Connection));
                }
            }

            try {
                (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
                    [connections = std::move(closeConnections)](winrt::Windows::Foundation::IAsyncAction) mutable {
                        for (auto& connection : connections) {
                            AudioConnectionService::Close(connection);
                        }
                    });
            } catch (winrt::hresult_error const& ex) {
                util::DebugTraceException(L"[DeviceManager] Power transition cleanup scheduling failed", ex);
            } catch (std::exception const& ex) {
                util::DebugTraceException(L"[DeviceManager] Power transition cleanup scheduling failed", ex);
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] Power transition cleanup scheduling failed");
            }
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

void DeviceManager::SetAutoReconnectPredicate(AutoReconnectPredicate pred) {
    auto guard = m_lock.lock_exclusive();
    m_autoReconnectPred = std::move(pred);
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    try {
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored during process exit: {0}", std::wstring(deviceId));
                co_return;
            }
            m_reconnectController.AllowReconnects();
            if (m_powerTransitionSuspended) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored during power transition suspend: {0}",
                           std::wstring(deviceId));
                co_return;
            }
            if (m_sessions.ConnectingIds().count(deviceId) > 0) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored; connect already running for {0}",
                           std::wstring(deviceId));
                co_return;
            }
            m_sessions.ConnectingIds().insert(deviceId);
        }
        DebugTrace(L"[DeviceManager] ConnectAsync requested: {0}", std::wstring(deviceId));

        // ID-only cache (no DeviceInformation caching anymore to avoid lifetime issues).
        bool knownDeviceId = false;
        knownDeviceId = m_discoveryService && m_discoveryService->ContainsDeviceId(deviceIdKey);

        auto devices = co_await m_discoveryService->RefreshAsync();
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                m_sessions.ConnectingIds().erase(deviceId);
                co_return;
            }
        }

        winrt::Windows::Devices::Enumeration::DeviceInformation targetDevice{nullptr};
        for (auto const& device : devices) {
            if (device.Id() == deviceId) {
                targetDevice = device;
                break;
            }
        }

        if (targetDevice) {
            co_await ConnectInternalAsync(targetDevice);
            {
                auto guard = m_lock.lock_exclusive();
                m_sessions.ConnectingIds().erase(deviceId);
            }
            DeviceActivityChanged(deviceId);
            co_return;
        }

        if (knownDeviceId) {
            DebugTrace(L"[DeviceManager] ConnectAsync known ID no longer present after refresh: {0}",
                       std::wstring(deviceId));
        }

        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, winrt::hstring(_("UnknownError")));
            DeviceStatusChanged(
                deviceId,
                winrt::hstring(_("UnknownError")),
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    } catch (winrt::hresult_error const& ex) {
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, ex.message());
            DeviceStatusChanged(
                deviceId,
                ex.message(),
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    } catch (std::exception const& ex) {
        auto message = winrt::hstring(util::Utf8ToUtf16(ex.what()));
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, message);
            DeviceStatusChanged(
                deviceId,
                message,
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ConnectAsync ERROR");
        auto message = winrt::hstring(_("UnknownError"));
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, message);
            DeviceStatusChanged(
                deviceId,
                message,
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    }
    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.ConnectingIds().erase(deviceId);
    }
    DeviceActivityChanged(deviceId);
}

void DeviceManager::ConnectDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        if (auto self = weak.lock()) {
            co_await self->ConnectAsync(id);
        }
    }(std::move(weak), std::move(deviceId));
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ReconnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    try {
        if (deviceId.empty()) co_return;
        DebugTrace(L"[DeviceManager] ReconnectAsync requested: {0}", std::wstring(deviceId));

        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored during process exit: {0}", std::wstring(deviceId));
                co_return;
            }
            m_reconnectController.AllowReconnects();
            if (m_powerTransitionSuspended) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored during power transition suspend: {0}",
                           std::wstring(deviceId));
                co_return;
            }
            if (m_sessions.ReconnectingIds().count(deviceId) > 0) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored; reconnect already running for {0}",
                           std::wstring(deviceId));
                co_return;
            }
            m_sessions.ReconnectingIds().insert(deviceId);
            m_reconnectController.BeginManualReconnect(deviceId);
        }

        DeviceStatusChanged(
            deviceId,
            winrt::hstring(_("Reconnecting")),
            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);

        // Extract the old connection and close it synchronously (on a background
        // thread) BEFORE creating the new one. If we let Close() run in parallel
        // with the new Connect (as the old zombie-list path did), the BT stack
        // sees an overlap and closes the newly opened connection immediately,
        // which then looks like an Unexpected disconnect and triggers a spurious
        // auto-reconnect cycle.
        winrt::Windows::Media::Audio::AudioPlaybackConnection oldConn{nullptr};
        winrt::event_token oldToken{};
        {
            auto guard = m_lock.lock_exclusive();
            auto iter = m_sessions.Connections().find(deviceId);
            if (iter != m_sessions.Connections().end()) {
                oldConn = std::move(iter->second.Connection);
                oldToken = iter->second.StateChangedToken;
                m_sessions.Connections().erase(iter);
                ++m_connectAttemptIds[std::wstring(deviceId)];
                m_sessions.DisconnectingIds().insert(deviceId);
                TrackUserActionCascadeLocked(deviceId);
            }
        }

        if (oldConn) {
            // Revoke the event token first so no stale Closed callbacks fire.
            if (oldToken.value != 0) {
                AudioConnectionService::RevokeStateChanged(oldConn, oldToken);
            }
            // Close on a background thread – never block the UI thread.
            co_await winrt::resume_background();
            DebugTrace(L"[DeviceManager] ReconnectAsync closing old connection: {0}", std::wstring(deviceId));
            bool shutdownForProcessExit = false;
            {
                auto guard = m_lock.lock_shared();
                shutdownForProcessExit = m_shutdownForProcessExit;
            }
            if (shutdownForProcessExit) {
                AudioConnectionService::DetachForProcessExit(oldConn);
            } else {
                AudioConnectionService::Close(oldConn);
                DebugTrace(L"[DeviceManager] ReconnectAsync old connection closed; cooling down before reconnect: {0}",
                           std::wstring(deviceId));
                co_await winrt::resume_after(ReconnectController::ReconnectCloseCooldown());
            }
        }

        {
            auto guard = m_lock.lock_exclusive();
            m_sessions.DisconnectingIds().erase(deviceId);
        }
        DeviceActivityChanged(deviceId);

        co_await ConnectAsync(deviceId);

    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: {0}", ex.message());
        ConnectionError(deviceId, ex.message());
        DeviceStatusChanged(deviceId,
                            ex.message(),
                            winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    } catch (std::exception const& ex) {
        auto message = winrt::hstring(util::Utf8ToUtf16(ex.what()));
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: {0}", std::wstring(message));
        ConnectionError(deviceId, message);
        DeviceStatusChanged(
            deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ReconnectAsync ERROR");
        auto message = winrt::hstring(_("UnknownError"));
        ConnectionError(deviceId, message);
        DeviceStatusChanged(
            deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.ReconnectingIds().erase(deviceId);
    }
    DeviceActivityChanged(deviceId);
    co_return;
}

void DeviceManager::ReconnectDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        if (auto self = weak.lock()) {
            co_await self->ReconnectAsync(id);
        }
    }(std::move(weak), std::move(deviceId));
}

void DeviceManager::Disconnect(winrt::hstring deviceId) {
    Disconnect(std::move(deviceId), DisconnectReason::UserInitiated);
}

void DeviceManager::SetAutoReconnect(winrt::hstring deviceId, bool enabled) {
    auto guard = m_lock.lock_exclusive();
    auto iter = m_sessions.Connections().find(deviceId);
    if (iter != m_sessions.Connections().end()) iter->second.AutoReconnect = enabled;
    if (!enabled) m_reconnectController.ClearAttempts(deviceId);
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

bool DeviceManager::HasConnections() const {
    auto guard = m_lock.lock_shared();
    return m_sessions.HasConnections();
}

bool DeviceManager::HasBusyOperations() const {
    auto guard = m_lock.lock_shared();
    return m_sessions.HasBusyOperations();
}

bool DeviceManager::IsDeviceBusy(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_sessions.IsDeviceBusy(deviceId);
}

void DeviceManager::StartConnectionHeartbeat() {
    if (m_heartbeatTimer) return;

    auto weak = weak_from_this();
    m_heartbeatTimer = winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
        [weak](auto) {
            if (auto self = weak.lock()) {
                self->LogConnectionSnapshot(L"heartbeat");
            }
        },
        std::chrono::minutes(c_heartbeatIntervalMinutes));

    LogConnectionSnapshot(L"heartbeat-started");
}

void DeviceManager::StopConnectionHeartbeat() {
    auto timer = std::exchange(m_heartbeatTimer, nullptr);
    if (timer) {
        try {
            timer.Cancel();
        } catch (...) {
        }
    }
    LogConnectionSnapshot(L"heartbeat-stopped");
}

void DeviceManager::LogConnectionSnapshot(winrt::hstring const& reason) const {
    struct Snapshot {
        winrt::hstring Id;
        winrt::hstring Name;
        bool HasConnection = false;
        bool IsOpen = false;
        bool AutoReconnect = false;
        bool Disconnecting = false;
        bool Reconnecting = false;
        bool CancelledReconnect = false;
        std::size_t ReconnectAttempts = 0;
        std::size_t ConnectAttemptId = 0;
    };

    std::vector<Snapshot> snapshots;
    bool allReconnectsCancelled = false;
    std::size_t deviceCacheSize = m_discoveryService ? m_discoveryService->CacheSize() : 0;
    {
        auto guard = m_lock.lock_shared();
        snapshots.reserve(m_sessions.Connections().size());
        allReconnectsCancelled = m_reconnectController.AllReconnectsCancelled();
        for (auto const& [id, info] : m_sessions.Connections()) {
            Snapshot snapshot;
            snapshot.Id = id;
            if (info.Device) {
                try {
                    snapshot.Name = info.Device.Name();
                } catch (winrt::hresult_error const&) {
                } catch (std::exception const&) {
                }
            }
            snapshot.HasConnection = static_cast<bool>(info.Connection);
            snapshot.IsOpen = info.IsOpen;
            snapshot.AutoReconnect = info.AutoReconnect;
            snapshot.Disconnecting = m_sessions.DisconnectingIds().count(id) > 0;
            snapshot.Reconnecting = m_sessions.ReconnectingIds().count(id) > 0;
            snapshot.CancelledReconnect = m_reconnectController.IsCancelled(id);
            snapshot.ReconnectAttempts = m_reconnectController.Attempts(id);
            if (auto iter = m_connectAttemptIds.find(std::wstring(id)); iter != m_connectAttemptIds.end()) {
                snapshot.ConnectAttemptId = iter->second;
            }
            snapshots.push_back(std::move(snapshot));
        }
    }

    DebugTrace(L"[DeviceManager] Snapshot reason={0} connections={1} cache={2} allReconnectsCancelled={3}",
               std::wstring(reason),
               snapshots.size(),
               deviceCacheSize,
               allReconnectsCancelled);

    for (auto const& snapshot : snapshots) {
        std::wstring state = L"<null>";
        if (snapshot.HasConnection) {
            // Avoid calling Connection.State() from the heartbeat timer thread.
            // We log the cached open-state instead to reduce cross-apartment
            // WinRT calls in diagnostics-only code.
            state = snapshot.IsOpen ? L"Opened(cached)" : L"Closed(cached)";
        }

        DebugTrace(
            L"[DeviceManager] Snapshot device id={0} name={1} isOpen={2} state={3} autoReconnect={4} disconnecting={5} "
            L"reconnecting={6} cancelledReconnect={7} reconnectAttempts={8} connectAttemptId={9}",
            std::wstring(snapshot.Id),
            std::wstring(snapshot.Name),
            snapshot.IsOpen,
            state,
            snapshot.AutoReconnect,
            snapshot.Disconnecting,
            snapshot.Reconnecting,
            snapshot.CancelledReconnect,
            snapshot.ReconnectAttempts,
            snapshot.ConnectAttemptId);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceManager::Disconnect(winrt::hstring deviceId, DisconnectReason reason) {
    auto reasonName = [](DisconnectReason value) -> std::wstring_view {
        switch (value) {
            case DisconnectReason::UserInitiated: return L"UserInitiated";
            case DisconnectReason::UserInitiatedCascade: return L"UserInitiatedCascade";
            case DisconnectReason::Unexpected: return L"Unexpected";
            case DisconnectReason::Cleanup: return L"Cleanup";
            default: return L"UnknownReason";
        }
    };
    DebugTrace(L"[DeviceManager] Disconnect requested: id={0} reason={1}", std::wstring(deviceId), reasonName(reason));

    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }

    bool autoReconnect = false;
    bool noActiveConnections = false;
    winrt::Windows::Media::Audio::AudioPlaybackConnection connection{nullptr};
    winrt::event_token stateChangedToken{};
    {
        auto guard = m_lock.lock_exclusive();
        auto iter = m_sessions.Connections().find(deviceId);
        if (iter == m_sessions.Connections().end()) return;

        connection = std::move(iter->second.Connection);
        stateChangedToken = iter->second.StateChangedToken;
        autoReconnect = iter->second.AutoReconnect;
        if (m_powerTransitionSuspended) {
            autoReconnect = false;
        }
        m_sessions.Connections().erase(iter);
        noActiveConnections = m_sessions.Connections().empty();
        ++m_connectAttemptIds[std::wstring(deviceId)];
        m_sessions.DisconnectingIds().insert(deviceId);
        if (reason == DisconnectReason::UserInitiated) {
            TrackUserActionCascadeLocked(deviceId);
            m_reconnectController.MarkUserCancelled(deviceId);
        }
    }

    if (noActiveConnections) StopConnectionHeartbeat();

    // Revoke the StateChanged token so the zombie cannot fire events at us.
    if (connection && stateChangedToken.value != 0) {
        AudioConnectionService::RevokeStateChanged(connection, stateChangedToken);
    }

    // Move the connection to the zombie list. Its destructor (which calls Close())
    // must not run on the UI thread, so we defer cleanup to a background thread.
    {
        auto guard = m_lock.lock_exclusive();
        if (connection) {
            m_sessions.ZombieConnections().push_back(std::move(connection));
        }
    }

    bool isReconnecting = false;
    {
        auto guard = m_lock.lock_shared();
        isReconnecting = m_sessions.ReconnectingIds().count(deviceId) > 0;
    }

    bool powerTransitionSuspended = false;
    {
        auto guard = m_lock.lock_shared();
        powerTransitionSuspended = m_powerTransitionSuspended;
    }

    if (reason != DisconnectReason::Cleanup && !isReconnecting && !powerTransitionSuspended) {
        DeviceDisconnected(deviceId);
        DeviceStatusChanged(
            deviceId, L"", winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None);
        if ((reason == DisconnectReason::Unexpected || reason == DisconnectReason::UserInitiatedCascade) &&
            autoReconnect)
            ScheduleReconnect(deviceId);
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.DisconnectingIds().erase(deviceId);
        if (reason == DisconnectReason::UserInitiated) {
            m_reconnectController.ClearUserCancellationIfNoTimer(deviceId);
        }
    }
    DeviceActivityChanged(deviceId);

    // Close all zombie connections on a background thread. If Close() blocks
    // (e.g. for old UI-thread-bound connections) it will not freeze the caller.
    std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> zombies;
    {
        auto guard = m_lock.lock_exclusive();
        zombies = std::move(m_sessions.ZombieConnections());
    }
    if (!zombies.empty()) {
        bool shutdownForProcessExit = false;
        {
            auto guard = m_lock.lock_shared();
            shutdownForProcessExit = m_shutdownForProcessExit;
        }
        if (shutdownForProcessExit) {
            for (auto& z : zombies) {
                AudioConnectionService::DetachForProcessExit(z);
            }
        } else {
            try {
                (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
                    [zombies = std::move(zombies)](winrt::Windows::Foundation::IAsyncAction) mutable {
                        for (auto& z : zombies) {
                            AudioConnectionService::Close(z);
                        }
                    });
            } catch (winrt::hresult_error const& ex) {
                util::DebugTraceException(L"[DeviceManager] Disconnect cleanup scheduling failed", ex);
            } catch (std::exception const& ex) {
                util::DebugTraceException(L"[DeviceManager] Disconnect cleanup scheduling failed", ex);
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] Disconnect cleanup scheduling failed");
            }
        }
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
    DeviceStatusChanged(
        deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
}

winrt::Windows::Foundation::IAsyncAction
DeviceManager::ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto lifetime = shared_from_this();
    auto deviceId = device.Id();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    std::size_t attemptId = 0;

    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) co_return;
        if (m_sessions.Connections().count(deviceId)) co_return;
        attemptId = ++m_connectAttemptIds[deviceIdKey];
    }

    // Create the connection on a background thread. If AudioPlaybackConnection is STA-bound,
    // this gives it its own COM apartment instead of marshalling everything to the UI thread.
    co_await winrt::resume_background();

    try {
        {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit) co_return;
        }

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
                ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), false);
            }
            co_return;
        }

        DeviceConnectionInfo info;
        info.Device = device;
        info.Connection = connection;
        info.IsOpen = false;
        auto weak = weak_from_this();
        info.StateChangedToken =
            AudioConnectionService::RegisterStateChanged(connection, [weak](auto sender, auto args) {
                if (auto self = weak.lock()) {
                    self->OnConnectionStateChanged(sender, args);
                }
            });
        AutoReconnectPredicate pred;
        {
            auto guard = m_lock.lock_shared();
            pred = m_autoReconnectPred;
        }
        info.AutoReconnect = pred ? pred(deviceId) : false;

        bool duplicateConnection = false;
        bool shutdownForProcessExit = false;
        {
            auto guard = m_lock.lock_exclusive();
            auto attempt = m_connectAttemptIds.find(deviceIdKey);
            shutdownForProcessExit = m_shutdownForProcessExit;
            if (shutdownForProcessExit || attempt == m_connectAttemptIds.end() || attempt->second != attemptId ||
                m_sessions.Connections().count(deviceId)) {
                duplicateConnection = true;
            } else {
                m_sessions.Connections()[deviceId] = std::move(info);
                m_reconnectController.ClearCancelled(deviceId);
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
            co_return;
        }

        bool isReconnecting = false;
        {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit) co_return;
            isReconnecting = m_sessions.ReconnectingIds().count(deviceId) > 0;
        }

        if (!isReconnecting) {
            DeviceStatusChanged(
                deviceId,
                winrt::hstring(_("Connecting")),
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress |
                    winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
        }

        co_await AudioConnectionService::StartAsync(connection);
        DebugTrace(L"[DeviceManager] StartAsync completed: {0}", std::wstring(deviceId));
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        auto result = co_await AudioConnectionService::OpenAsync(connection);
        DebugTrace(L"[DeviceManager] OpenAsync completed: id={0} status={1} extended=0x{2:08X}",
                   std::wstring(deviceId),
                   OpenResultStatusName(result.Status()),
                   static_cast<uint32_t>(result.ExtendedError()));
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        bool currentAttempt = false;
        {
            auto guard = m_lock.lock_exclusive();
            auto attempt = m_connectAttemptIds.find(deviceIdKey);
            currentAttempt = !m_shutdownForProcessExit && attempt != m_connectAttemptIds.end() &&
                             attempt->second == attemptId && m_sessions.Connections().count(deviceId) > 0;
        }
        if (!currentAttempt) co_return;

        switch (result.Status()) {
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: {
                {
                    auto guard = m_lock.lock_exclusive();
                    auto iter = m_sessions.Connections().find(deviceId);
                    auto attempt = m_connectAttemptIds.find(deviceIdKey);
                    if (m_shutdownForProcessExit || iter == m_sessions.Connections().end() ||
                        attempt == m_connectAttemptIds.end() || attempt->second != attemptId)
                        co_return;
                    iter->second.IsOpen = true;
                    m_reconnectController.ClearAttempts(deviceId);
                    m_userActionCascadeIds.erase(deviceId);
                }
                DeviceConnected(deviceId);
                DeviceStatusChanged(
                    deviceId,
                    winrt::hstring(_("Connected")),
                    winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
                LogConnectionSnapshot(L"open-success");
                StartConnectionHeartbeat();
                break;
            }
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
                ReportConnectionFailure(deviceId, winrt::hstring(_("RequestTimedOut")), true);
                break;
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
                ReportConnectionFailure(deviceId, winrt::hstring(_("DeniedBySystem")), true);
                break;
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure: {
                winrt::hresult_error err(result.ExtendedError());
                ReportConnectionFailure(deviceId, err.message(), true);
                break;
            }
        }
    } catch (winrt::hresult_error const& ex) {
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ReportConnectionFailure(deviceId, ex.message(), true);
    } catch (std::exception const& ex) {
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ReportConnectionFailure(deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), true);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ConnectInternalAsync ERROR");
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), true);
    }
}

bool DeviceManager::IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const {
    auto guard = m_lock.lock_shared();
    if (m_shutdownForProcessExit) return false;
    const std::wstring deviceIdKey = std::wstring(deviceId);
    auto attempt = m_connectAttemptIds.find(deviceIdKey);
    return attempt != m_connectAttemptIds.end() && attempt->second == attemptId;
}

void DeviceManager::TrackUserActionCascadeLocked(winrt::hstring const& deviceId) {
    auto now = std::chrono::steady_clock::now();
    PruneUserActionCascadeLocked(now);

    bool marked = false;
    auto const expiresAt = now + c_userActionCascadeWindow;
    for (auto const& [id, info] : m_sessions.Connections()) {
        if (id == deviceId || !info.IsOpen) continue;
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

    auto iter = m_userActionCascadeIds.find(deviceId);
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
    if (m_discoveryDeviceAddedToken && m_discoveryDeviceRemovedToken) return;

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
}

void DeviceManager::OnConnectionStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
                                             winrt::Windows::Foundation::IInspectable) {
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }

    winrt::hstring id;
    try {
        auto state = sender.State();
        id = sender.DeviceId();
        DebugTrace(L"[DeviceManager] StateChanged: id={0} state={1}", std::wstring(id), ConnectionStateName(state));
        if (state != winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed) return;
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

    bool isUserActionCascade = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_sessions.DisconnectingIds().count(id)) return;
        if (m_sessions.ReconnectingIds().count(id)) return; // reconnect in progress – ignore stale Closed events

        auto iter = m_sessions.Connections().find(id);
        if (iter == m_sessions.Connections().end()) return;

        // Ignore stale callbacks from an older connection object that was already replaced.
        if (!iter->second.Connection || iter->second.Connection != sender) return;
        isUserActionCascade = ConsumeUserActionCascadeLocked(id);
    }

    if (isUserActionCascade) {
        DebugTrace(L"[DeviceManager] StateChanged Closed treated as user-action cascade: {0}", std::wstring(id));
        Disconnect(id, DisconnectReason::UserInitiatedCascade);
    } else {
        Disconnect(id, DisconnectReason::Unexpected);
    }
}

void DeviceManager::ScheduleReconnect(winrt::hstring deviceId) {
    try {
        ReconnectController::ScheduleDecision decision;
        {
            auto guard = m_lock.lock_exclusive();
            decision =
                m_reconnectController.PrepareSchedule(deviceId, m_shutdownForProcessExit || m_powerTransitionSuspended);
        }

        if (decision.NotifyFailed) {
            DebugTrace(L"[DeviceManager] Auto-reconnect stopped after {0} attempts for {1}",
                       decision.MaxAttempts,
                       std::wstring(deviceId));
            ConnectionError(deviceId, winrt::hstring(_("AutoReconnectFailed")));
            DeviceStatusChanged(
                deviceId,
                winrt::hstring(_("AutoReconnectFailed")),
                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
            AutoReconnectFailed(deviceId);
            return;
        }
        if (!decision.ShouldSchedule) return;

        DebugTrace(L"[DeviceManager] Auto-reconnect scheduled: id={0} attempt={1} delaySeconds={2}",
                   std::wstring(deviceId),
                   decision.Attempt,
                   decision.Delay.count());
        AutoReconnectTriggered(deviceId);
        // Erase the stale-cancel marker set by Disconnect() so this new timer
        // is allowed to fire. Any timer created before this Disconnect() call
        // already read the marker as cancelled (it was inserted first).
        {
            auto guard = m_lock.lock_exclusive();
            m_reconnectController.StartTimer(deviceId);
        }
        auto weak = weak_from_this();
        (void)winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [weak, deviceId](auto) {
                if (auto self = weak.lock()) {
                    bool cancelled = false;
                    {
                        auto guard = self->m_lock.lock_shared();
                        cancelled =
                            self->m_reconnectController.ShouldSkipTimer(deviceId, self->m_shutdownForProcessExit);
                    }
                    if (!cancelled) {
                        self->ConnectDetached(deviceId);
                    }
                    {
                        auto guard = self->m_lock.lock_exclusive();
                        self->m_reconnectController.CompleteTimer(deviceId);
                    }
                }
            },
            decision.Delay);
    } catch (winrt::hresult_error const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (std::exception const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (...) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId);
        util::DebugTraceUnknownException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer");
    }
}

void DeviceManager::OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceInformation args) {
    AutoReconnectPredicate pred;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        pred = m_autoReconnectPred;
    }

    if (!pred) return;
    if (pred(args.Id())) {
        ConnectDetached(args.Id());
    }
}

void DeviceManager::OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args) {
    bool connectedDeviceRemoved = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;

        connectedDeviceRemoved = m_sessions.Connections().count(args.Id()) > 0 &&
                                 m_sessions.DisconnectingIds().count(args.Id()) == 0 &&
                                 m_sessions.ReconnectingIds().count(args.Id()) == 0;
    }

    if (connectedDeviceRemoved) {
        Disconnect(args.Id(), DisconnectReason::Unexpected);
    }
}
