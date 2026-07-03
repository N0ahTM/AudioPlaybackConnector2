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
constexpr std::chrono::milliseconds c_cascadeRestorePollInterval{500};
constexpr std::chrono::milliseconds c_cascadeRestoreSettleDelay{2500};
constexpr int c_cascadeRestoreMaxBusyWaits = 20;
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
                                        std::wstring_view context) noexcept {
    if (connections.empty()) return;

    auto sharedConnections =
        std::make_shared<std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>>(std::move(connections));
    auto closeConnections = [](std::shared_ptr<std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>>
                                   connectionsToClose) noexcept {
        for (auto& connection : *connectionsToClose) {
            AudioConnectionService::Close(connection);
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
            auto zombies = m_sessions.TakeZombieConnections();
            connections.reserve(allConnections.size() + zombies.size());

            for (auto& [id, info] : allConnections) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : zombies) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_sessions.Clear();
            m_reconnectController.ClearTracking();
            m_connectAttemptIds.clear();
            m_userActionCascadeIds.clear();
            m_cascadeRestoreIds.clear();
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
            for (auto& entry : m_connectAttemptIds) {
                ++entry.second;
            }
            auto allConnections = m_sessions.ExtractAllConnections();
            auto zombies = m_sessions.TakeZombieConnections();
            connections.reserve(allConnections.size() + zombies.size());

            for (auto& [id, info] : allConnections) {
                if (info.Connection) {
                    connections.push_back({std::move(info.Connection), info.StateChangedToken});
                }
            }

            for (auto& connection : zombies) {
                if (connection) {
                    connections.push_back({std::move(connection), {}});
                }
            }

            m_sessions.Clear();
            m_reconnectController.ClearTracking();
            m_userActionCascadeIds.clear();
            m_cascadeRestoreIds.clear();
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

            CloseConnectionsOnBackgroundThread(std::move(closeConnections), L"Power transition cleanup");
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
        m_cascadeRestoreIds.clear();
    }
    DebugTrace(L"[DeviceManager] Power transition resume completed");
    StartDeviceWatcher();
}

void DeviceManager::CancelPendingReconnects() {
    auto guard = m_lock.lock_exclusive();
    m_reconnectController.CancelPendingReconnects();
    m_userActionCascadeIds.clear();
    m_cascadeRestoreIds.clear();
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
            if (m_sessions.HasConnection(deviceId)) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored; device already connected: {0}",
                           std::wstring(deviceId));
                co_return;
            }
            if (m_sessions.IsConnecting(deviceId)) {
                DebugTrace(L"[DeviceManager] ConnectAsync ignored; connect already running for {0}",
                           std::wstring(deviceId));
                co_return;
            }
            m_sessions.MarkConnecting(deviceId);
        }
        DebugTrace(L"[DeviceManager] ConnectAsync requested: {0}", std::wstring(deviceId));

        // ID-only cache (no DeviceInformation caching anymore to avoid lifetime issues).
        bool knownDeviceId = false;
        knownDeviceId = m_discoveryService && m_discoveryService->ContainsDeviceId(deviceIdKey);

        auto devices = co_await m_discoveryService->RefreshAsync();
        {
            auto guard = m_lock.lock_exclusive();
            if (m_shutdownForProcessExit || m_powerTransitionSuspended) {
                m_sessions.UnmarkConnecting(deviceId);
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
                m_sessions.UnmarkConnecting(deviceId);
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
            DeviceStatusChanged(deviceId,
                                winrt::hstring(_("UnknownError")),
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
    } catch (winrt::hresult_error const& ex) {
        bool reportFailure = false;
        {
            auto guard = m_lock.lock_shared();
            reportFailure = !m_shutdownForProcessExit;
        }
        if (reportFailure) {
            ConnectionError(deviceId, ex.message());
            DeviceStatusChanged(deviceId,
                                ex.message(),
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
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
            DeviceStatusChanged(deviceId,
                                message,
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
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
            DeviceStatusChanged(deviceId,
                                message,
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
        }
    }
    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.UnmarkConnecting(deviceId);
    }
    DeviceActivityChanged(deviceId);
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
            if (m_sessions.IsReconnecting(deviceId)) {
                DebugTrace(L"[DeviceManager] ReconnectAsync ignored; reconnect already running for {0}",
                           std::wstring(deviceId));
                co_return;
            }
            m_sessions.MarkReconnecting(deviceId);
            m_reconnectController.BeginManualReconnect(deviceId);
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
        {
            auto guard = m_lock.lock_exclusive();
            auto extracted = m_sessions.ExtractConnection(deviceId);
            if (extracted) {
                oldConn = std::move(extracted->Connection);
                oldToken = extracted->StateChangedToken;
                ++m_connectAttemptIds[std::wstring(deviceId)];
                m_sessions.MarkDisconnecting(deviceId);
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
            m_sessions.UnmarkDisconnecting(deviceId);
        }
        DeviceActivityChanged(deviceId);

        co_await ConnectAsync(deviceId);

    } catch (winrt::hresult_error const& ex) {
        ReportAsyncConnectionError(*this, deviceId, ex.message(), L"ReconnectAsync");
    } catch (std::exception const& ex) {
        ReportAsyncConnectionError(*this, deviceId, winrt::hstring(util::Utf8ToUtf16(ex.what())), L"ReconnectAsync");
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceManager] ReconnectAsync ERROR");
        ReportAsyncConnectionError(*this, deviceId, winrt::hstring(_("UnknownError")), L"ReconnectAsync");
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.UnmarkReconnecting(deviceId);
    }
    DeviceActivityChanged(deviceId);
    co_return;
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

void DeviceManager::SetAutoReconnect(winrt::hstring deviceId, bool enabled) {
    auto guard = m_lock.lock_exclusive();
    m_sessions.SetConnectionAutoReconnect(deviceId, enabled);
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

void DeviceManager::StartConnectionHeartbeat() {
    bool started = false;
    {
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
    }
    LogConnectionSnapshot(L"heartbeat-stopped");
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
            snapshot.AutoReconnect = info.AutoReconnect;
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
        auto extracted = m_sessions.ExtractConnection(deviceId);
        if (!extracted) return;

        connection = std::move(extracted->Connection);
        stateChangedToken = extracted->StateChangedToken;
        autoReconnect = extracted->AutoReconnect;
        if (m_powerTransitionSuspended) {
            autoReconnect = false;
        }
        noActiveConnections = m_sessions.ConnectionCount() == 0;
        ++m_connectAttemptIds[std::wstring(deviceId)];
        m_sessions.MarkDisconnecting(deviceId);
        if (reason == DisconnectReason::UserInitiated && !suppressCascade) {
            TrackUserActionCascadeLocked(deviceId);
            m_reconnectController.MarkUserCancelled(deviceId);
        }
    }

    if (noActiveConnections) {
        StopConnectionHeartbeat();
    }

    // Revoke the StateChanged token so the zombie cannot fire events at us.
    if (connection && stateChangedToken.value != 0) {
        AudioConnectionService::RevokeStateChanged(connection, stateChangedToken);
    }

    // Move the connection to the zombie list. Its destructor (which calls Close())
    // must not run on the UI thread, so we defer cleanup to a background thread.
    {
        auto guard = m_lock.lock_exclusive();
        if (connection) {
            m_sessions.AddZombie(std::move(connection));
        }
    }

    bool isReconnecting = false;
    {
        auto guard = m_lock.lock_shared();
        isReconnecting = m_sessions.IsReconnecting(deviceId);
    }

    bool powerTransitionSuspended = false;
    {
        auto guard = m_lock.lock_shared();
        powerTransitionSuspended = m_powerTransitionSuspended;
    }

    if (reason != DisconnectReason::Cleanup && !isReconnecting && !powerTransitionSuspended) {
        if (reason == DisconnectReason::UserInitiatedCascade) {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            RestoreCascadeConnectionDetached(deviceId);
        } else {
            DeviceDisconnected(deviceId);
            DeviceStatusChanged(deviceId,
                                L"",
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::None,
                                DeviceStatusKind::None);
            if (reason == DisconnectReason::Unexpected && autoReconnect) {
                ScheduleReconnect(deviceId);
            }
        }
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_sessions.UnmarkDisconnecting(deviceId);
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
        zombies = m_sessions.TakeZombieConnections();
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
            CloseConnectionsOnBackgroundThread(std::move(zombies), L"Disconnect cleanup");
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
    DeviceStatusChanged(deviceId,
                        message,
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                        DeviceStatusKind::Error);
}

winrt::Windows::Foundation::IAsyncAction
DeviceManager::ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device) {
    auto lifetime = shared_from_this();
    auto deviceId = device.Id();
    const std::wstring deviceIdKey = std::wstring(deviceId);
    std::size_t attemptId = 0;

    DeviceConnectionInfo metadataTemplate;
    PopulateConnectionMetadata(metadataTemplate, device);

    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit) co_return;
        if (m_sessions.HasConnection(deviceId)) co_return;
        attemptId = ++m_connectAttemptIds[deviceIdKey];
    }

    // Create the connection on a background thread. If AudioPlaybackConnection is STA-bound,
    // this gives it its own COM apartment instead of marshalling everything to the UI thread.
    co_await winrt::resume_background();

    try {
        {
            auto guard = m_lock.lock_shared();
            if (m_shutdownForProcessExit || m_powerTransitionSuspended) co_return;
        }

        auto closePendingConnectionForRetry =
            [&](winrt::Windows::Media::Audio::AudioPlaybackConnection& failedConnection) -> bool {
            std::optional<DeviceConnectionInfo> extracted;
            {
                auto guard = m_lock.lock_exclusive();
                auto attempt = m_connectAttemptIds.find(deviceIdKey);
                if (m_shutdownForProcessExit || m_powerTransitionSuspended || attempt == m_connectAttemptIds.end() ||
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
                if (shutdownForProcessExit || m_powerTransitionSuspended || attempt == m_connectAttemptIds.end() ||
                    attempt->second != attemptId || m_sessions.HasConnection(deviceId)) {
                    duplicateConnection = true;
                } else {
                    m_sessions.InsertOrUpdateConnection(deviceId, std::move(info));
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
                if (m_shutdownForProcessExit || m_powerTransitionSuspended) co_return;
                isReconnecting = m_sessions.IsReconnecting(deviceId);
            }

            if (!isReconnecting) {
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
            if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

            auto result = co_await AudioConnectionService::OpenAsync(connection);
            DebugTrace(L"[DeviceManager] OpenAsync completed: id={0} openAttempt={1} status={2} extended=0x{3:08X}",
                       std::wstring(deviceId),
                       openAttempt,
                       DeviceOpenResultStatusName(result.Status()),
                       static_cast<uint32_t>(result.ExtendedError()));
            if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;

            bool currentAttempt = false;
            {
                auto guard = m_lock.lock_exclusive();
                auto attempt = m_connectAttemptIds.find(deviceIdKey);
                currentAttempt = !m_shutdownForProcessExit && !m_powerTransitionSuspended &&
                                 attempt != m_connectAttemptIds.end() && attempt->second == attemptId &&
                                 m_sessions.HasConnection(deviceId);
            }
            if (!currentAttempt) co_return;

            switch (result.Status()) {
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: {
                    {
                        auto guard = m_lock.lock_exclusive();
                        auto existingInfo = m_sessions.FindConnection(deviceId);
                        auto attempt = m_connectAttemptIds.find(deviceIdKey);
                        if (m_shutdownForProcessExit || m_powerTransitionSuspended || !existingInfo ||
                            attempt == m_connectAttemptIds.end() || attempt->second != attemptId)
                            co_return;
                        m_sessions.UpdateConnectionIsOpen(deviceId, true);
                        m_sessions.UnmarkReconnecting(deviceId);
                        m_reconnectController.ClearAttempts(deviceId);
                        m_userActionCascadeIds.erase(deviceIdKey);
                    }
                    isReconnecting = false;
                    DeviceConnected(deviceId);
                    DeviceStatusChanged(
                        deviceId,
                        winrt::hstring(_("Connected")),
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowDisconnectButton,
                        DeviceStatusKind::Connected);
                    LogConnectionSnapshot(L"open-success");
                    StartConnectionHeartbeat();
                    co_return;
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
                        if (!closePendingConnectionForRetry(connection)) co_return;
                        co_await winrt::resume_after(delay);
                        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
                        continue;
                    }
                    ReportConnectionFailure(deviceId, winrt::hstring(_("RequestTimedOut")), true);
                    co_return;
                case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
                    ReportConnectionFailure(deviceId, winrt::hstring(_("DeniedBySystem")), true);
                    co_return;
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
                        if (!closePendingConnectionForRetry(connection)) co_return;
                        co_await winrt::resume_after(delay);
                        if (!IsConnectAttemptCurrent(deviceId, attemptId)) co_return;
                        continue;
                    }
                    winrt::hresult_error err(result.ExtendedError());
                    ReportConnectionFailure(deviceId, err.message(), true);
                    co_return;
                }
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
    if (m_shutdownForProcessExit || m_powerTransitionSuspended) return false;
    const std::wstring deviceIdKey = std::wstring(deviceId);
    auto attempt = m_connectAttemptIds.find(deviceIdKey);
    return attempt != m_connectAttemptIds.end() && attempt->second == attemptId;
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

void DeviceManager::RestoreCascadeConnectionDetached(winrt::hstring deviceId) {
    const std::wstring deviceIdKey = std::wstring(deviceId);
    {
        auto guard = m_lock.lock_exclusive();
        if (m_shutdownForProcessExit || m_powerTransitionSuspended || m_sessions.HasConnection(deviceId)) {
            return;
        }
        if (!m_cascadeRestoreIds.insert(deviceIdKey).second) {
            return;
        }
        m_sessions.MarkReconnecting(deviceId);
    }

    DebugTrace(L"[DeviceManager] Cascade restore scheduled: {0}", std::wstring(deviceId));
    DeviceStatusChanged(deviceId,
                        winrt::hstring(_("Reconnecting")),
                        winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowProgress,
                        DeviceStatusKind::Reconnecting);
    DeviceActivityChanged(deviceId);

    auto weak = weak_from_this();
    [](std::weak_ptr<DeviceManager> weak, winrt::hstring id) -> winrt::fire_and_forget {
        auto self = weak.lock();
        if (!self) co_return;

        auto clearPending = wil::scope_exit([self, id]() noexcept {
            try {
                bool wasMarked = false;
                {
                    auto guard = self->m_lock.lock_exclusive();
                    self->m_cascadeRestoreIds.erase(std::wstring(id));
                    wasMarked = self->m_sessions.IsReconnecting(id);
                    if (wasMarked) {
                        self->m_sessions.UnmarkReconnecting(id);
                    }
                }
                if (wasMarked) {
                    self->DeviceActivityChanged(id);
                }
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceManager] Cascade restore clear pending ignored exception");
            }
        });

        try {
            co_await winrt::resume_after(ReconnectController::ReconnectCloseCooldown());

            for (int waitCount = 0; waitCount < c_cascadeRestoreMaxBusyWaits; ++waitCount) {
                bool shouldWait = false;
                bool shouldStop = false;
                {
                    auto guard = self->m_lock.lock_shared();
                    shouldStop = self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                                 self->m_sessions.HasConnection(id);
                    shouldWait =
                        self->m_sessions.HasBusyOperationsExcept(id) || self->m_reconnectController.HasPendingTimers();
                }
                if (shouldStop) co_return;
                if (!shouldWait) break;
                co_await winrt::resume_after(c_cascadeRestorePollInterval);
            }

            co_await winrt::resume_after(c_cascadeRestoreSettleDelay);

            bool wasMarked = false;
            {
                auto guard = self->m_lock.lock_exclusive();
                if (self->m_shutdownForProcessExit || self->m_powerTransitionSuspended ||
                    self->m_sessions.HasConnection(id)) {
                    co_return;
                }
                if (self->m_sessions.IsReconnecting(id)) {
                    self->m_sessions.UnmarkReconnecting(id);
                    wasMarked = true;
                }
            }
            if (wasMarked) {
                self->DeviceActivityChanged(id);
            }

            DebugTrace(L"[DeviceManager] Cascade restore starting: {0}", std::wstring(id));
            co_await self->ConnectAsync(id);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceManager] Cascade restore ignored exception");
        }
    }(std::move(weak), std::move(deviceId));
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

void DeviceManager::OnConnectionStateChanged(winrt::hstring deviceId,
                                             winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
                                             winrt::Windows::Foundation::IInspectable) {
    {
        auto guard = m_lock.lock_shared();
        if (m_shutdownForProcessExit) return;
    }

    try {
        auto state = sender.State();
        DebugTrace(L"[DeviceManager] StateChanged: id={0} state={1}",
                   std::wstring(deviceId),
                   DeviceConnectionStateName(state));
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
        isUserActionCascade = ConsumeUserActionCascadeLocked(deviceId);
    }

    if (isUserActionCascade) {
        DebugTrace(L"[DeviceManager] StateChanged Closed treated as user-action cascade: {0}", std::wstring(deviceId));
        Disconnect(deviceId, DisconnectReason::UserInitiatedCascade);
    } else {
        Disconnect(deviceId, DisconnectReason::Unexpected);
    }
}

void DeviceManager::ScheduleReconnect(winrt::hstring deviceId) {
    std::size_t timerGeneration = 0;
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
            DeviceStatusChanged(deviceId,
                                winrt::hstring(_("AutoReconnectFailed")),
                                winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions::ShowRetryButton,
                                DeviceStatusKind::Error);
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
            timerGeneration = m_reconnectController.StartTimer(deviceId);
        }
        auto weak = weak_from_this();
        (void)winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [weak, deviceId, timerGeneration](auto) {
                if (auto self = weak.lock()) {
                    bool cancelled = false;
                    {
                        auto guard = self->m_lock.lock_shared();
                        cancelled = self->m_reconnectController.ShouldSkipTimer(
                                        deviceId, self->m_shutdownForProcessExit, timerGeneration) ||
                                    self->m_sessions.HasConnection(deviceId) ||
                                    self->m_sessions.IsConnecting(deviceId) ||
                                    self->m_sessions.IsReconnecting(deviceId);
                    }
                    if (!cancelled) {
                        self->ConnectDetached(deviceId);
                    }
                    {
                        auto guard = self->m_lock.lock_exclusive();
                        self->m_reconnectController.CompleteTimer(deviceId, timerGeneration);
                    }
                    self->DeviceActivityChanged(deviceId);
                }
            },
            decision.Delay);
    } catch (winrt::hresult_error const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId, timerGeneration);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (std::exception const& ex) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId, timerGeneration);
        util::DebugTraceException(L"[DeviceManager] ScheduleReconnect ERROR: failed to create reconnect timer", ex);
    } catch (...) {
        auto guard = m_lock.lock_exclusive();
        m_reconnectController.HandleTimerCreateFailed(deviceId, timerGeneration);
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

        connectedDeviceRemoved = m_sessions.HasConnection(args.Id()) && !m_sessions.IsDisconnecting(args.Id()) &&
                                 !m_sessions.IsReconnecting(args.Id());
    }

    if (connectedDeviceRemoved) {
        Disconnect(args.Id(), DisconnectReason::Unexpected);
    }
}
