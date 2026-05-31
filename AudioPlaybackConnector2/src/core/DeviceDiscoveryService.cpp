#include <pch.h>

#include <core/DeviceDiscoveryService.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DeviceDiscoveryService::~DeviceDiscoveryService() {
    Stop();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceDiscoveryService::Start() {
    if (m_watcher) return;

    try {
        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        m_watcher = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateWatcher(selector);
        m_watcherAddedToken =
            m_watcher.Added([this](auto const& sender, auto const& args) { OnDeviceAdded(sender, args); });
        m_watcherRemovedToken =
            m_watcher.Removed([this](auto const& sender, auto const& args) { OnDeviceRemoved(sender, args); });
        m_watcher.Start();
        DebugTrace(L"[DeviceDiscoveryService] DeviceWatcher started");
    } catch (...) {
        DebugTrace(L"[DeviceDiscoveryService] Start ERROR: failed to create or start watcher");
    }
}

void DeviceDiscoveryService::Stop() {
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
        DebugTrace(L"[DeviceDiscoveryService] Stop ERROR: failed to stop watcher or revoke token");
    }

    m_watcher = nullptr;
    {
        auto guard = m_lock.lock_exclusive();
        m_watcherStopping = false;
    }
}

void DeviceDiscoveryService::ClearCache() {
    auto guard = m_lock.lock_exclusive();
    m_deviceCache.clear();
}

bool DeviceDiscoveryService::ContainsDeviceId(std::wstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_deviceCache.count(deviceId) > 0;
}

std::size_t DeviceDiscoveryService::CacheSize() const {
    auto guard = m_lock.lock_shared();
    return m_deviceCache.size();
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
DeviceDiscoveryService::RefreshAsync() {
    auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
    auto devices = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);

    std::unordered_set<std::wstring> refreshed;
    refreshed.reserve(static_cast<size_t>(devices.Size()));
    for (auto const& device : devices) {
        refreshed.insert(std::wstring(device.Id()));
    }

    {
        auto guard = m_lock.lock_exclusive();
        m_deviceCache = std::move(refreshed);
    }

    co_return devices;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceDiscoveryService::OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceWatcher const&,
                                           winrt::Windows::Devices::Enumeration::DeviceInformation const& args) {
    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherStopping) return;
        m_deviceCache.insert(std::wstring(args.Id()));
    }
    DeviceAdded(args);
}

void DeviceDiscoveryService::OnDeviceRemoved(
    winrt::Windows::Devices::Enumeration::DeviceWatcher const&,
    winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& args) {
    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherStopping) return;
        m_deviceCache.erase(std::wstring(args.Id()));
    }
    DeviceRemoved(args);
}
