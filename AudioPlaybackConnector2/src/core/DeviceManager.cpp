#include <pch.h>
#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <utility>

namespace {
constexpr std::size_t c_maxReconnectAttempts = 10;
constexpr int c_initialReconnectDelaySeconds = 5;
constexpr int c_maxReconnectDelaySeconds = 60;
constexpr int c_heartbeatIntervalMinutes = 5;
constexpr std::chrono::milliseconds c_reconnectCloseCooldown{1500};

std::chrono::seconds GetReconnectDelay(std::size_t attempt) {
    auto delay = c_initialReconnectDelaySeconds;
    for (std::size_t i = 1; i < attempt && delay < c_maxReconnectDelaySeconds; ++i) {
        delay = std::min(delay * 2, c_maxReconnectDelaySeconds);
    }
    return std::chrono::seconds(delay);
}

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
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut: return L"RequestTimedOut";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem: return L"DeniedBySystem";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure: return L"UnknownFailure";
        default: return L"UnknownStatus";
    }
}

void DetachConnectionForProcessExit(winrt::Windows::Media::Audio::AudioPlaybackConnection& connection) noexcept {
    if (!connection) return;
    try {
        auto leaked = winrt::detach_abi(connection);
        (void)leaked;
    } catch (...) {
    }
}

} // namespace

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DeviceManager::StartDeviceWatcher() {
    if (m_watcher) return;
    try {
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) return;
            m_allReconnectsCancelled = false;
        }
        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        m_watcher = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateWatcher(selector);
        auto weak = weak_from_this();
        m_watcherAddedToken = m_watcher.Added([weak](auto sender, auto args) {
            if (auto self = weak.lock()) {
                self->OnDeviceAdded(sender, args);
            }
        });
        m_watcherRemovedToken = m_watcher.Removed([weak](auto sender, auto args) {
            if (auto self = weak.lock()) {
                self->OnDeviceRemoved(sender, args);
            }
        });
        m_watcher.Start();
        DebugTrace(L"[DeviceManager] DeviceWatcher started");
    } catch (...) {
        DebugTrace(L"[DeviceManager] StartDeviceWatcher ERROR: failed to create or start watcher");
    }
}

void DeviceManager::StopDeviceWatcher() {
    StopConnectionHeartbeat();
    if (!m_watcher) return;
    {
        auto guard = m_lock.lock_exclusive();
        m_watcherStopping = true;
    }
    try {
        m_watcher.Stop();
        m_watcher.Added(std::exchange(m_watcherAddedToken, {}));
        m_watcher.Removed(std::exchange(m_watcherRemovedToken, {}));
    } catch (...) {
        DebugTrace(L"[DeviceManager] StopDeviceWatcher ERROR: failed to stop watcher or revoke token");
    }
    m_watcher = nullptr;
    {
        auto guard = m_lock.lock_exclusive();
        m_watcherStopping = false;
    }
}

void DeviceManager::ShutdownForProcessExit() noexcept {
    try {
        {
            auto guard = m_lock.lock_exclusive();
            m_shutdownForProcessExit = true;
            m_allReconnectsCancelled = true;
            for (auto& entry : m_connectAttemptIds) {
                ++entry.second;
            }
        }

        StopDeviceWatcher();
        CancelPendingReconnects();

        struct ConnectionForShutdown {
            winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
            winrt::event_token StateChangedToken{};
        };
        std::vector<ConnectionForShutdown> connections;
        {
            auto guard = m_lock.lock_exclusive();
            connections.reserve(m_connections.size() + m_zombieConnections.size());

            for (auto& [id, info] : m_connections) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : m_zombieConnections) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_connections.clear();
            m_zombieConnections.clear();
            m_deviceCache.clear();
            m_disconnectingIds.clear();
            m_reconnectingIds.clear();
            m_connectingIds.clear();
            m_cancelledReconnectIds.clear();
            m_reconnectTimerCounts.clear();
            m_reconnectAttempts.clear();
            m_connectAttemptIds.clear();
            m_autoReconnectPred = nullptr;
        }

        if (!connections.empty()) {
            DebugTrace(L"[DeviceManager] Shutdown detached {0} connection object(s) for process teardown", connections.size());
            for (auto& item : connections) {
                if (item.Connection && item.StateChangedToken.value != 0) {
                    try {
                        item.Connection.StateChanged(item.StateChangedToken);
                    } catch (...) {
                    }
                }
                DetachConnectionForProcessExit(item.Connection);
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
            connections.reserve(m_connections.size() + m_zombieConnections.size());

            for (auto& [id, info] : m_connections) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : m_zombieConnections) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_connections.clear();
            m_zombieConnections.clear();
            m_disconnectingIds.clear();
            m_reconnectingIds.clear();
            m_connectingIds.clear();
            m_cancelledReconnectIds.clear();
            m_reconnectTimerCounts.clear();
            m_reconnectAttempts.clear();
        }

        if (!connections.empty()) {
            std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> closeConnections;
            closeConnections.reserve(connections.size());
            for (auto& item : connections) {
                if (item.Connection && item.StateChangedToken.value != 0) {
                    try {
                        item.Connection.StateChanged(item.StateChangedToken);
                    } catch (...) {
                    }
                }
                if (item.Connection) {
                    closeConnections.push_back(std::move(item.Connection));
                }
            }

            try {
                (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
                    [connections = std::move(closeConnections)](winrt::Windows::Foundation::IAsyncAction) mutable {
                        for (auto& connection : connections) {
                            try {
                                connection.Close();
                            } catch (...) {
                            }
                        }
                    });
            } catch (...) {
                DebugTrace(L"[DeviceManager] Power transition cleanup scheduling failed");
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
        m_allReconnectsCancelled = false;
        m_cancelledReconnectIds.clear();
    }
    DebugTrace(L"[DeviceManager] Power transition resume completed");
    StartDeviceWatcher();
}

void DeviceManager::CancelPendingReconnects() {
    auto guard = m_lock.lock_exclusive();
    m_allReconnectsCancelled = true;
    m_reconnectAttempts.clear();
    m_cancelledReconnectIds.clear();
    DebugTrace(L"[DeviceManager] Pending reconnects cancelled");
}

void DeviceManager::SetAutoReconnectPredicate(AutoReconnectPredicate pred) {
    auto guard = m_lock.lock_exclusive();
    m_autoReconnectPred = std::move(pred);
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    try {
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored during process exit: {0}", std::wstring(deviceId));
                co_return;
            }
            m_allReconnectsCancelled = false;
            if (m_powerTransitionSuspended) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored during power transition suspend: {0}", std::wstring(deviceId));
                co_return;
            }
            if (m_connectingIds.count(deviceId) > 0) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored; connect already running for {0}", std::wstring(deviceId));
                co_return;
            }
            m_connectingIds.insert(deviceId);
        }
        DebugTrace(L"[DeviceManager] ConnectAsync requested: {0}", std::wstring(deviceId));
        // Check cache first to avoid an expensive FindAllAsync scan.
        winrt::Windows::Devices::Enumeration::DeviceInformation cachedDevice{nullptr};
        {
            auto guard = m_lock.lock_shared();
            for (auto const& cached : m_deviceCache) {
                if (cached.Id() == deviceId) {
                    cachedDevice = cached;
                    break;
                }
            }
        }
        if (cachedDevice) {
            co_await ConnectInternalAsync(cachedDevice);
            {
                auto guard = m_lock.lock_exclusive();
                m_connectingIds.erase(deviceId);
            }
            co_return;
        }

        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        auto devices = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                m_connectingIds.erase(deviceId);
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

        // Refresh cache with the latest device enumeration snapshot.
        {
            std::vector<winrt::Windows::Devices::Enumeration::DeviceInformation> refreshed;
            refreshed.reserve(devices.Size());
            for (auto const& device : devices) {
                refreshed.push_back(device);
            }
            auto guard = m_lock.lock_exclusive();
            m_deviceCache = std::move(refreshed);
        }

        if (targetDevice) {
            co_await ConnectInternalAsync(targetDevice);
            {
                auto guard = m_lock.lock_exclusive();
                m_connectingIds.erase(deviceId);
            }
            co_return;
        }

        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, winrt::hstring(_("UnknownError")));
            DeviceStatusChanged(deviceId, winrt::hstring(_("UnknownError")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    } catch (winrt::hresult_error const& ex) {
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, ex.message());
            DeviceStatusChanged(deviceId, ex.message(), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    } catch (...) {
        auto message = winrt::hstring(_("UnknownError"));
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, message);
            DeviceStatusChanged(deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
        }
    }
    {
        auto guard = m_lock.lock_exclusive();
        m_connectingIds.erase(deviceId);
    }
}

void DeviceManager::ConnectDetached(winrt::hstring deviceId) {
    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        if (auto self = weak.lock()) {
            co_await self->ConnectAsync(id);
        }
    }(std::move(weak), std::move(deviceId));
}

winrt::fire_and_forget DeviceManager::ReconnectAsync(winrt::hstring deviceId) {
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
            m_allReconnectsCancelled = false;
            if (m_powerTransitionSuspended) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored during power transition suspend: {0}", std::wstring(deviceId));
                co_return;
            }
            if (m_reconnectingIds.count(deviceId) > 0) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored; reconnect already running for {0}", std::wstring(deviceId));
                co_return;
            }
            m_reconnectingIds.insert(deviceId);
            m_cancelledReconnectIds.erase(deviceId);
            m_reconnectAttempts.erase(deviceId);
        }

        DeviceStatusChanged(deviceId, winrt::hstring(_("Reconnecting")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress | winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);

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
            auto iter = m_connections.find(deviceId);
            if (iter != m_connections.end()) {
                oldConn = std::move(iter->second.Connection);
                oldToken = iter->second.StateChangedToken;
                m_connections.erase(iter);
                ++m_connectAttemptIds[deviceId];
                m_disconnectingIds.insert(deviceId);
            }
        }

        if (oldConn) {
            // Revoke the event token first so no stale Closed callbacks fire.
            if (oldToken.value != 0) {
                try {
                    oldConn.StateChanged(oldToken);
                } catch (...) {
                }
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
                DetachConnectionForProcessExit(oldConn);
            } else {
                try {
                    oldConn.Close();
                } catch (...) {
                }
                oldConn = nullptr;
                DebugTrace(L"[DeviceManager] ReconnectAsync old connection closed; cooling down before reconnect: {0}", std::wstring(deviceId));
                co_await winrt::resume_after(c_reconnectCloseCooldown);
            }
        }

        {
            auto guard = m_lock.lock_exclusive();
            m_disconnectingIds.erase(deviceId);
        }

        co_await ConnectAsync(deviceId);

    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: {0}", ex.message());
        ConnectionError(deviceId, ex.message());
        DeviceStatusChanged(deviceId, ex.message(), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    } catch (...) {
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: Unknown exception");
        auto message = winrt::hstring(_("UnknownError"));
        ConnectionError(deviceId, message);
        DeviceStatusChanged(deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_reconnectingIds.erase(deviceId);
    }
    co_return;
}

void DeviceManager::Disconnect(winrt::hstring deviceId) {
    Disconnect(std::move(deviceId), DisconnectReason::UserInitiated);
}

void DeviceManager::SetAutoReconnect(winrt::hstring deviceId, bool enabled) {
    auto guard = m_lock.lock_exclusive();
    auto iter = m_connections.find(deviceId);
    if (iter != m_connections.end())
        iter->second.AutoReconnect = enabled;
    if (!enabled)
        m_reconnectAttempts.erase(deviceId);
}

std::vector<DeviceConnectionInfo> DeviceManager::GetConnectedDevices() const {
    auto guard = m_lock.lock_shared();
    std::vector<DeviceConnectionInfo> result;
    result.reserve(m_connections.size());
    for (const auto& entry : m_connections) {
        if (entry.second.IsOpen)
            result.push_back(entry.second);
    }
    return result;
}

bool DeviceManager::HasConnections() const {
    auto guard = m_lock.lock_shared();
    return std::ranges::any_of(m_connections, [](auto const& entry) { return entry.second.IsOpen; });
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
        winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
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
    std::size_t deviceCacheSize = 0;
    {
        auto guard = m_lock.lock_shared();
        snapshots.reserve(m_connections.size());
        allReconnectsCancelled = m_allReconnectsCancelled;
        deviceCacheSize = m_deviceCache.size();
        for (auto const& [id, info] : m_connections) {
            Snapshot snapshot;
            snapshot.Id = id;
            if (info.Device) {
                try {
                    snapshot.Name = info.Device.Name();
                } catch (...) {
                }
            }
            snapshot.Connection = info.Connection;
            snapshot.IsOpen = info.IsOpen;
            snapshot.AutoReconnect = info.AutoReconnect;
            snapshot.Disconnecting = m_disconnectingIds.count(id) > 0;
            snapshot.Reconnecting = m_reconnectingIds.count(id) > 0;
            snapshot.CancelledReconnect = m_cancelledReconnectIds.count(id) > 0;
            if (auto iter = m_reconnectAttempts.find(id); iter != m_reconnectAttempts.end()) {
                snapshot.ReconnectAttempts = iter->second;
            }
            if (auto iter = m_connectAttemptIds.find(id); iter != m_connectAttemptIds.end()) {
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
        try {
            if (snapshot.Connection) {
                state = std::wstring(ConnectionStateName(snapshot.Connection.State()));
            }
        } catch (winrt::hresult_error const& ex) {
            state = std::format(L"StateError(0x{:08X} {})", static_cast<uint32_t>(ex.code()), std::wstring(ex.message()));
        } catch (...) {
            state = L"StateError(unknown)";
        }

        DebugTrace(L"[DeviceManager] Snapshot device id={0} name={1} isOpen={2} state={3} autoReconnect={4} disconnecting={5} reconnecting={6} cancelledReconnect={7} reconnectAttempts={8} connectAttemptId={9}",
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

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DeviceManager::Disconnect(winrt::hstring deviceId, DisconnectReason reason) {
    auto reasonName = [](DisconnectReason value) -> std::wstring_view {
        switch (value) {
            case DisconnectReason::UserInitiated: return L"UserInitiated";
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
        auto iter = m_connections.find(deviceId);
        if (iter == m_connections.end()) return;

        connection = std::move(iter->second.Connection);
        stateChangedToken = iter->second.StateChangedToken;
        autoReconnect = iter->second.AutoReconnect;
        if (m_powerTransitionSuspended) {
            autoReconnect = false;
        }
        m_connections.erase(iter);
        noActiveConnections = m_connections.empty();
        ++m_connectAttemptIds[deviceId];
        m_disconnectingIds.insert(deviceId);
        if (reason == DisconnectReason::UserInitiated) {
            m_cancelledReconnectIds.insert(deviceId);
            m_reconnectAttempts.erase(deviceId);
        }
    }

    if (noActiveConnections)
        StopConnectionHeartbeat();

    // Revoke the StateChanged token so the zombie cannot fire events at us.
    if (connection && stateChangedToken.value != 0) {
        try {
            connection.StateChanged(stateChangedToken);
        } catch (...) {
        }
    }

    // Move the connection to the zombie list. Its destructor (which calls Close())
    // must not run on the UI thread, so we defer cleanup to a background thread.
    {
        auto guard = m_lock.lock_exclusive();
        if (connection) {
            m_zombieConnections.push_back(std::move(connection));
        }
    }

    bool isReconnecting = false;
    {
        auto guard = m_lock.lock_shared();
        isReconnecting = m_reconnectingIds.count(deviceId) > 0;
    }

    bool powerTransitionSuspended = false;
    {
        auto guard = m_lock.lock_shared();
        powerTransitionSuspended = m_powerTransitionSuspended;
    }

    if (reason != DisconnectReason::Cleanup && !isReconnecting && !powerTransitionSuspended) {
        DeviceDisconnected(deviceId);
        DeviceStatusChanged(deviceId, L"", winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None);
        if (reason == DisconnectReason::Unexpected && autoReconnect)
            ScheduleReconnect(deviceId);
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_disconnectingIds.erase(deviceId);
        if (reason == DisconnectReason::UserInitiated && m_reconnectTimerCounts[deviceId] == 0) {
            m_reconnectTimerCounts.erase(deviceId);
            m_cancelledReconnectIds.erase(deviceId);
        }
    }

    // Close all zombie connections on a background thread. If Close() blocks
    // (e.g. for old UI-thread-bound connections) it will not freeze the caller.
    std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> zombies;
    {
        auto guard = m_lock.lock_exclusive();
        zombies = std::move(m_zombieConnections);
    }
    if (!zombies.empty()) {
        bool shutdownForProcessExit = false;
        {
            auto guard = m_lock.lock_shared();
            shutdownForProcessExit = m_shutdownForProcessExit;
        }
        if (shutdownForProcessExit) {
            for (auto& z : zombies) {
                DetachConnectionForProcessExit(z);
            }
        } else {
            try {
                (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
                    [zombies = std::move(zombies)](winrt::Windows::Foundation::IAsyncAction) mutable {
                        for (auto& z : zombies) {
                            try {
                                z.Close();
                            } catch (...) {
                            }
                        }
                    });
            } catch (...) {
                DebugTrace(L"[DeviceManager] Disconnect cleanup scheduling failed");
            }
        }
    }

    LogConnectionSnapshot(winrt::hstring(L"disconnect:") + winrt::hstring(reasonName(reason)));
}

void DeviceManager::ReportConnectionFailure(winrt::hstring const& deviceId, winrt::hstring const& message, bool cleanupConnection) {
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }
    ConnectionError(deviceId, message);
    if (cleanupConnection) {
        Disconnect(deviceId, DisconnectReason::Cleanup);
    }
    DeviceStatusChanged(deviceId, message, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto lifetime = shared_from_this();
    auto deviceId = device.Id();
    std::size_t attemptId = 0;

    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) co_return;
        if (m_connections.count(deviceId)) co_return;
        attemptId = ++m_connectAttemptIds[deviceId];
    }

    // Create the connection on a background thread. If AudioPlaybackConnection is STA-bound,
    // this gives it its own COM apartment instead of marshalling everything to the UI thread.
    co_await winrt::resume_background();

    try {
        {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit) co_return;
        }

        auto connection = winrt::Windows::Media::Audio::AudioPlaybackConnection::TryCreateFromId(deviceId);
        auto detachConnectionOnExitShutdown = wil::scope_exit([&]() noexcept {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit) {
                DetachConnectionForProcessExit(connection);
            }
        });

        if (!connection) {
            // Do NOT touch m_reconnectingIds here – ReconnectAsync owns that flag
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
        info.StateChangedToken = connection.StateChanged(
            [weak](auto sender, auto args) {
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
            auto attempt = m_connectAttemptIds.find(deviceId);
            shutdownForProcessExit = m_shutdownForProcessExit;
            if (shutdownForProcessExit || attempt == m_connectAttemptIds.end() || attempt->second != attemptId || m_connections.count(deviceId)) {
                duplicateConnection = true;
            } else {
                m_connections[deviceId] = std::move(info);
                m_cancelledReconnectIds.erase(deviceId);
            }
        }
        if (duplicateConnection) {
            if (!shutdownForProcessExit && info.StateChangedToken.value != 0) {
                try {
                    connection.StateChanged(info.StateChangedToken);
                } catch (...) {
                }
            }
            if (shutdownForProcessExit) {
                DetachConnectionForProcessExit(connection);
            } else {
                try {
                    connection.Close();
                } catch (...) {
                }
                connection = nullptr;
            }
            co_return;
        }

        bool isReconnecting = false;
        {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit) co_return;
            isReconnecting = m_reconnectingIds.count(deviceId) > 0;
        }

        if (!isReconnecting) {
            DeviceStatusChanged(deviceId, winrt::hstring(_("Connecting")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress | winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
        }

        co_await connection.StartAsync();
        DebugTrace(L"[DeviceManager] StartAsync completed: {0}", std::wstring(deviceId));
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        auto result = co_await connection.OpenAsync();
        DebugTrace(L"[DeviceManager] OpenAsync completed: id={0} status={1} extended=0x{2:08X}",
                   std::wstring(deviceId),
                   OpenResultStatusName(result.Status()),
                   static_cast<uint32_t>(result.ExtendedError()));
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        bool currentAttempt = false;
        {
            auto guard = m_lock.lock_exclusive();
            auto attempt = m_connectAttemptIds.find(deviceId);
            currentAttempt = !m_shutdownForProcessExit &&
                             attempt != m_connectAttemptIds.end() &&
                             attempt->second == attemptId &&
                             m_connections.count(deviceId) > 0;
        }
        if (!currentAttempt) co_return;

        switch (result.Status()) {
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: {
                {
                    auto guard = m_lock.lock_exclusive();
                    auto iter = m_connections.find(deviceId);
                    auto attempt = m_connectAttemptIds.find(deviceId);
                    if (m_shutdownForProcessExit || iter == m_connections.end() || attempt == m_connectAttemptIds.end() || attempt->second != attemptId) co_return;
                    iter->second.IsOpen = true;
                    m_reconnectAttempts.erase(deviceId);
                }
                DeviceConnected(deviceId);
                DeviceStatusChanged(deviceId, winrt::hstring(_("Connected")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
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
                winrt::hresult_error err(result.ExtendedError(), winrt::take_ownership_from_abi);
                ReportConnectionFailure(deviceId, err.message(), true);
                break;
            }
        }
    } catch (winrt::hresult_error const& ex) {
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ReportConnectionFailure(deviceId, ex.message(), true);
    } catch (...) {
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ReportConnectionFailure(deviceId, winrt::hstring(_("UnknownError")), true);
    }
}

bool DeviceManager::IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const {
    auto guard = m_lock.lock_shared();
    if (m_shutdownForProcessExit) return false;
    auto attempt = m_connectAttemptIds.find(deviceId);
    return attempt != m_connectAttemptIds.end() &&
           attempt->second == attemptId;
}

void DeviceManager::OnConnectionStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection sender, winrt::Windows::Foundation::IInspectable) {
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
        DebugTrace(L"[DeviceManager] StateChanged callback failed: 0x{0:X} {1}", static_cast<uint32_t>(ex.code()), ex.message());
        return;
    } catch (...) {
        DebugTrace(L"[DeviceManager] StateChanged callback failed: unknown exception");
        return;
    }

    {
        auto guard = m_lock.lock_shared();
        if (m_disconnectingIds.count(id)) return;
        if (m_reconnectingIds.count(id)) return; // reconnect in progress – ignore stale Closed events

        auto iter = m_connections.find(id);
        if (iter == m_connections.end()) return;

        // Ignore stale callbacks from an older connection object that was already replaced.
        if (!iter->second.Connection || iter->second.Connection != sender) return;
    }

    Disconnect(id, DisconnectReason::Unexpected);
}

void DeviceManager::ScheduleReconnect(winrt::hstring deviceId) {
    try {
        std::size_t attempt = 0;
        bool notifyFailed = false;
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit) {
                m_cancelledReconnectIds.insert(deviceId);
                return;
            }
            if (m_powerTransitionSuspended) {
                m_cancelledReconnectIds.insert(deviceId);
                return;
            }
            auto& attempts = m_reconnectAttempts[deviceId];
            if (attempts >= c_maxReconnectAttempts) {
                notifyFailed = attempts == c_maxReconnectAttempts;
                attempts = c_maxReconnectAttempts + 1;
                m_cancelledReconnectIds.insert(deviceId);
            } else {
                attempt = ++attempts;
            }
        }

        if (notifyFailed) {
            DebugTrace(L"[DeviceManager] Auto-reconnect stopped after {0} attempts for {1}", c_maxReconnectAttempts, std::wstring(deviceId));
            ConnectionError(deviceId, winrt::hstring(_("AutoReconnectFailed")));
            DeviceStatusChanged(deviceId, winrt::hstring(_("AutoReconnectFailed")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
            AutoReconnectFailed(deviceId);
            return;
        }

        DebugTrace(L"[DeviceManager] Auto-reconnect scheduled: id={0} attempt={1} delaySeconds={2}",
                   std::wstring(deviceId),
                   attempt,
                   GetReconnectDelay(attempt).count());
        AutoReconnectTriggered(deviceId);
        // Erase the stale-cancel marker set by Disconnect() so this new timer
        // is allowed to fire. Any timer created before this Disconnect() call
        // already read the marker as cancelled (it was inserted first).
        {
            auto guard = m_lock.lock_exclusive();
            m_cancelledReconnectIds.erase(deviceId);
            ++m_reconnectTimerCounts[deviceId];
        }
        auto weak = weak_from_this();
        auto timer = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [weak, deviceId](auto) {
                if (auto self = weak.lock()) {
                    bool cancelled = false;
                    {
                        auto guard = self->m_lock.lock_shared();
                        cancelled = self->m_shutdownForProcessExit ||
                                    self->m_allReconnectsCancelled ||
                                    self->m_cancelledReconnectIds.count(deviceId) > 0;
                    }
                    if (!cancelled) {
                        self->ConnectDetached(deviceId);
                    }
                    {
                        auto guard = self->m_lock.lock_exclusive();
                        auto iter = self->m_reconnectTimerCounts.find(deviceId);
                        if (iter != self->m_reconnectTimerCounts.end()) {
                            if (iter->second > 1) {
                                --iter->second;
                            } else {
                                self->m_reconnectTimerCounts.erase(iter);
                                self->m_cancelledReconnectIds.erase(deviceId);
                            }
                        }
                    }
                }
            },
            GetReconnectDelay(attempt));
    } catch (...) {
        auto guard = m_lock.lock_exclusive();
        auto iter = m_reconnectTimerCounts.find(deviceId);
        if (iter != m_reconnectTimerCounts.end()) {
            if (iter->second > 1)
                --iter->second;
            else
                m_reconnectTimerCounts.erase(iter);
        }
        DebugTrace(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer for {0}", std::wstring(deviceId));
    }
}

void DeviceManager::OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceWatcher, winrt::Windows::Devices::Enumeration::DeviceInformation args) {
    AutoReconnectPredicate pred;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        if (m_watcherStopping) return;
        bool exists = false;
        for (auto const& cached : m_deviceCache) {
            if (cached.Id() == args.Id()) {
                exists = true;
                break;
            }
        }
        if (!exists) m_deviceCache.push_back(args);
        pred = m_autoReconnectPred;
    }

    if (!pred) return;
    if (pred(args.Id())) {
        ConnectDetached(args.Id());
    }
}

void DeviceManager::OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceWatcher, winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args) {
    bool connectedDeviceRemoved = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) return;
        if (m_watcherStopping) return;

        std::erase_if(m_deviceCache, [&](auto const& cached) { return cached.Id() == args.Id(); });

        connectedDeviceRemoved =
            m_connections.count(args.Id()) > 0 &&
            m_disconnectingIds.count(args.Id()) == 0 &&
            m_reconnectingIds.count(args.Id()) == 0;
    }

    if (connectedDeviceRemoved) {
        Disconnect(args.Id(), DisconnectReason::Unexpected);
    }
}
