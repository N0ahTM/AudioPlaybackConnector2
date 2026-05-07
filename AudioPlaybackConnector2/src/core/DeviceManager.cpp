#include <pch.h>
#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <utility>

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DeviceManager::StartDeviceWatcher() {
    if (m_watcher) return;
    try {
        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        m_watcher = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateWatcher(selector);
        m_watcherAddedToken = m_watcher.Added([this](auto sender, auto args) { OnDeviceAdded(sender, args); });
        m_watcherRemovedToken = m_watcher.Removed([this](auto sender, auto args) { OnDeviceRemoved(sender, args); });
        m_watcher.Start();
    } catch (...) {
        DebugTrace(L"[DeviceManager] StartDeviceWatcher ERROR: failed to create or start watcher");
    }
}

void DeviceManager::StopDeviceWatcher() {
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

void DeviceManager::CancelPendingReconnects() {
    auto guard = m_lock.lock_exclusive();
    m_allReconnectsCancelled = true;
}

void DeviceManager::SetAutoReconnectPredicate(AutoReconnectPredicate pred) {
    auto guard = m_lock.lock_exclusive();
    m_autoReconnectPred = std::move(pred);
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    try {
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
            co_return;
        }

        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        auto devices = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);

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
            co_return;
        }

        ConnectionError(deviceId, winrt::hstring(_("UnknownError")));
        DeviceStatusChanged(deviceId, winrt::hstring(_("UnknownError")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    } catch (winrt::hresult_error const& ex) {
        ConnectionError(deviceId, ex.message());
        DeviceStatusChanged(deviceId, ex.message(), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    }
}

winrt::fire_and_forget DeviceManager::ReconnectAsync(winrt::hstring deviceId) {
    auto lifetime = shared_from_this();
    try {
        {
            auto guard = m_lock.lock_exclusive();
            m_reconnectingIds.insert(deviceId);
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
            try {
                oldConn.Close();
            } catch (...) {
            }
            oldConn = nullptr;
        }

        {
            auto guard = m_lock.lock_exclusive();
            m_disconnectingIds.erase(deviceId);
        }

        co_await ConnectAsync(deviceId);

    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: {0}", ex.message());
        ConnectionError(deviceId, ex.message());
    } catch (...) {
        DebugTrace(L"[DeviceManager] ReconnectAsync ERROR: Unknown exception");
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
    return std::any_of(m_connections.begin(), m_connections.end(), [](auto const& entry) { return entry.second.IsOpen; });
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DeviceManager::Disconnect(winrt::hstring deviceId, DisconnectReason reason) {
    bool autoReconnect = false;
    winrt::Windows::Media::Audio::AudioPlaybackConnection connection{nullptr};
    winrt::event_token stateChangedToken{};
    {
        auto guard = m_lock.lock_exclusive();
        auto iter = m_connections.find(deviceId);
        if (iter == m_connections.end()) return;

        connection = std::move(iter->second.Connection);
        stateChangedToken = iter->second.StateChangedToken;
        autoReconnect = iter->second.AutoReconnect;
        m_connections.erase(iter);
        ++m_connectAttemptIds[deviceId];
        m_disconnectingIds.insert(deviceId);
        if (reason == DisconnectReason::UserInitiated) {
            m_cancelledReconnectIds.insert(deviceId);
        }
    }

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

    if (reason != DisconnectReason::Cleanup && !isReconnecting) {
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
        (void)winrt::Windows::System::Threading::ThreadPool::RunAsync(
            [zombies = std::move(zombies)](winrt::Windows::Foundation::IAsyncAction) mutable {
                for (auto& z : zombies) {
                    try {
                        z.Close();
                    } catch (...) {
                    }
                }
            });
    }
}

winrt::Windows::Foundation::IAsyncAction DeviceManager::ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto lifetime = shared_from_this();
    auto deviceId = device.Id();
    std::size_t attemptId = 0;

    {
        auto guard = m_lock.lock_exclusive();
        if (m_connections.count(deviceId)) co_return;
        attemptId = ++m_connectAttemptIds[deviceId];
    }

    // Create the connection on a background thread. If AudioPlaybackConnection is STA-bound,
    // this gives it its own COM apartment instead of marshalling everything to the UI thread.
    co_await winrt::resume_background();

    try {
        auto connection = winrt::Windows::Media::Audio::AudioPlaybackConnection::TryCreateFromId(deviceId);
        if (!connection) {
            // Do NOT touch m_reconnectingIds here – ReconnectAsync owns that flag
            // for the entire reconnect flow and will clear it when finished.
            ConnectionError(deviceId, winrt::hstring(_("UnknownError")));
            DeviceStatusChanged(deviceId, winrt::hstring(_("UnknownError")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
            co_return;
        }

        DeviceConnectionInfo info;
        info.Device = device;
        info.Connection = connection;
        info.IsOpen = false;
        info.StateChangedToken = connection.StateChanged(
            [this, deviceId](auto sender, auto) { OnConnectionStateChanged(sender, nullptr); });
        AutoReconnectPredicate pred;
        {
            auto guard = m_lock.lock_shared();
            pred = m_autoReconnectPred;
        }
        info.AutoReconnect = pred ? pred(deviceId) : false;

        bool duplicateConnection = false;
        {
            auto guard = m_lock.lock_exclusive();
            if (m_connectAttemptIds[deviceId] != attemptId || m_connections.count(deviceId)) {
                duplicateConnection = true;
            } else {
                m_connections[deviceId] = std::move(info);
                m_cancelledReconnectIds.erase(deviceId);
            }
        }
        if (duplicateConnection) {
            if (info.StateChangedToken.value != 0) {
                try {
                    connection.StateChanged(info.StateChangedToken);
                } catch (...) {
                }
            }
            try {
                connection.Close();
            } catch (...) {
            }
            co_return;
        }

        bool isReconnecting = false;
        {
            auto guard = m_lock.lock_shared();
            isReconnecting = m_reconnectingIds.count(deviceId) > 0;
        }

        if (!isReconnecting) {
            DeviceStatusChanged(deviceId, winrt::hstring(_("Connecting")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress | winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);
        }

        co_await connection.StartAsync();
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        auto result = co_await connection.OpenAsync();
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

        bool currentAttempt = false;
        {
            auto guard = m_lock.lock_exclusive();
            currentAttempt = m_connectAttemptIds[deviceId] == attemptId &&
                             m_connections.count(deviceId) > 0;
        }
        if (!currentAttempt) co_return;

        switch (result.Status()) {
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: {
                {
                    auto guard = m_lock.lock_exclusive();
                    auto iter = m_connections.find(deviceId);
                    if (iter == m_connections.end() || m_connectAttemptIds[deviceId] != attemptId) co_return;
                    iter->second.IsOpen = true;
                }
                DeviceConnected(deviceId);
                DeviceStatusChanged(deviceId, winrt::hstring(_("Connected")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton);

                break;
            }
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
                ConnectionError(deviceId, winrt::hstring(_("RequestTimedOut")));
                Disconnect(deviceId, DisconnectReason::Cleanup);
                DeviceStatusChanged(deviceId, winrt::hstring(_("RequestTimedOut")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
                break;
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
                ConnectionError(deviceId, winrt::hstring(_("DeniedBySystem")));
                Disconnect(deviceId, DisconnectReason::Cleanup);
                DeviceStatusChanged(deviceId, winrt::hstring(_("DeniedBySystem")), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
                break;
            case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure: {
                winrt::hresult_error err(result.ExtendedError(), winrt::take_ownership_from_abi);
                ConnectionError(deviceId, err.message());
                Disconnect(deviceId, DisconnectReason::Cleanup);
                DeviceStatusChanged(deviceId, err.message(), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
                break;
            }
        }
    } catch (winrt::hresult_error const& ex) {
        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
        ConnectionError(deviceId, ex.message());
        Disconnect(deviceId, DisconnectReason::Cleanup);
        DeviceStatusChanged(deviceId, ex.message(), winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton);
    }
}

bool DeviceManager::IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const {
    auto guard = m_lock.lock_shared();
    auto attempt = m_connectAttemptIds.find(deviceId);
    return attempt != m_connectAttemptIds.end() &&
           attempt->second == attemptId;
}

void DeviceManager::OnConnectionStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection sender, winrt::Windows::Foundation::IInspectable) {
    if (sender.State() != winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed) return;

    auto id = sender.DeviceId();
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
                        cancelled = self->m_allReconnectsCancelled ||
                                    self->m_cancelledReconnectIds.count(deviceId) > 0;
                    }
                    if (!cancelled) {
                        (void)self->ConnectAsync(deviceId);
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
            std::chrono::seconds(5));
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
        (void)ConnectAsync(args.Id());
    }
}

void DeviceManager::OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceWatcher, winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args) {
    auto guard = m_lock.lock_exclusive();
    if (m_watcherStopping) return;
    m_deviceCache.erase(
        std::remove_if(m_deviceCache.begin(), m_deviceCache.end(), [&](auto const& cached) { return cached.Id() == args.Id(); }),
        m_deviceCache.end());
}
