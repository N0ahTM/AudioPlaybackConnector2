#include <pch.h>

#include <core/DeviceDiscoveryService.hpp>

namespace {

apc::device_picker::DeviceInventorySnapshot
BuildInventorySnapshot(std::uint64_t generation,
                       bool enumerationComplete,
                       std::unordered_map<std::wstring, std::wstring> const& deviceCache) {
    apc::device_picker::DeviceInventorySnapshot result;
    result.Generation = generation;
    result.EnumerationComplete = enumerationComplete;
    result.Devices.reserve(deviceCache.size());
    for (auto const& [id, name] : deviceCache) {
        result.Devices.push_back({id, name});
    }
    std::ranges::sort(result.Devices, [](auto const& left, auto const& right) {
        auto const leftName = left.Name.empty() ? std::wstring_view(left.Id) : std::wstring_view(left.Name);
        auto const rightName = right.Name.empty() ? std::wstring_view(right.Id) : std::wstring_view(right.Name);
        if (leftName != rightName) return leftName < rightName;
        return left.Id < right.Id;
    });
    return result;
}

} // namespace

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
    std::unique_lock lifecycleLock(m_watcherLifecycleMutex);
    if (m_watcher) return;

    std::uint64_t watcherGeneration = 0;
    {
        auto guard = m_lock.lock_exclusive();
        watcherGeneration = ++m_watcherGeneration;
        m_watcherStopping = false;
        m_enumerationComplete = false;
        m_deviceCache.clear();
        ++m_inventoryGeneration;
    }

    winrt::Windows::Devices::Enumeration::DeviceWatcher watcher{nullptr};
    winrt::event_token addedToken{};
    winrt::event_token removedToken{};
    winrt::event_token enumerationCompletedToken{};
    auto cleanupWatcher = [&]() noexcept {
        if (!watcher) return;
        try {
            watcher.Stop();
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceDiscoveryService] failed to stop rejected watcher");
        }
        try {
            if (addedToken.value != 0) watcher.Added(addedToken);
            if (removedToken.value != 0) watcher.Removed(removedToken);
            if (enumerationCompletedToken.value != 0) watcher.EnumerationCompleted(enumerationCompletedToken);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceDiscoveryService] failed to revoke rejected watcher tokens");
        }
    };
    std::uint64_t rejectedGeneration = 0;
    auto rejectWatcherGeneration = [&]() noexcept {
        try {
            if (m_watcher == watcher) {
                m_watcher = nullptr;
                m_watcherAddedToken = {};
                m_watcherRemovedToken = {};
                m_watcherEnumerationCompletedToken = {};
            }
            auto guard = m_lock.lock_exclusive();
            if (m_watcherGeneration != watcherGeneration) return;
            m_watcherStopping = true;
            const bool inventoryChanged = !m_deviceCache.empty() || m_enumerationComplete;
            m_enumerationComplete = false;
            m_deviceCache.clear();
            if (inventoryChanged) ++m_inventoryGeneration;
            rejectedGeneration = ++m_watcherGeneration;
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceDiscoveryService] failed to reject watcher generation");
        }
    };
    auto finishWatcherRejection = [&]() noexcept {
        if (rejectedGeneration == 0) return;
        try {
            auto guard = m_lock.lock_exclusive();
            if (m_watcherGeneration == rejectedGeneration) m_watcherStopping = false;
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceDiscoveryService] failed to finish watcher rejection");
        }
    };
    try {
        auto weak = weak_from_this();
        auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        watcher = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateWatcher(selector);
        addedToken = watcher.Added([weak, watcherGeneration](auto const& sender, auto const& args) {
            if (auto self = weak.lock()) {
                self->OnDeviceAdded(watcherGeneration, sender, args);
            }
        });
        removedToken = watcher.Removed([weak, watcherGeneration](auto const& sender, auto const& args) {
            if (auto self = weak.lock()) {
                self->OnDeviceRemoved(watcherGeneration, sender, args);
            }
        });
        enumerationCompletedToken = watcher.EnumerationCompleted([weak, watcherGeneration](auto const& sender, auto) {
            if (auto self = weak.lock()) {
                self->OnEnumerationCompleted(watcherGeneration, sender);
            }
        });
        watcher.Start();
        m_watcher = watcher;
        m_watcherAddedToken = addedToken;
        m_watcherRemovedToken = removedToken;
        m_watcherEnumerationCompletedToken = enumerationCompletedToken;
        DebugTrace(L"[DeviceDiscoveryService] DeviceWatcher started");
    } catch (winrt::hresult_error const& ex) {
        rejectWatcherGeneration();
        cleanupWatcher();
        finishWatcherRejection();
        util::DebugTraceException(L"[DeviceDiscoveryService] Start ERROR: failed to create or start watcher", ex);
    } catch (std::exception const& ex) {
        rejectWatcherGeneration();
        cleanupWatcher();
        finishWatcherRejection();
        util::DebugTraceException(L"[DeviceDiscoveryService] Start ERROR: failed to create or start watcher", ex);
    } catch (...) {
        rejectWatcherGeneration();
        cleanupWatcher();
        finishWatcherRejection();
        util::DebugTraceUnknownException(L"[DeviceDiscoveryService] Start ERROR: failed to create or start watcher");
    }
    lifecycleLock.unlock();
    try {
        InventoryChanged();
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceDiscoveryService] inventory reset notification failed");
    }
}

void DeviceDiscoveryService::Stop() {
    std::lock_guard lifecycleLock(m_watcherLifecycleMutex);
    if (!m_watcher) return;
    auto watcher = std::exchange(m_watcher, nullptr);
    auto addedToken = std::exchange(m_watcherAddedToken, {});
    auto removedToken = std::exchange(m_watcherRemovedToken, {});
    auto enumerationCompletedToken = std::exchange(m_watcherEnumerationCompletedToken, {});
    std::uint64_t stoppedGeneration = 0;
    {
        auto guard = m_lock.lock_exclusive();
        stoppedGeneration = ++m_watcherGeneration;
        m_watcherStopping = true;
        m_enumerationComplete = false;
        ++m_inventoryGeneration;
    }

    try {
        watcher.Stop();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DeviceDiscoveryService] Stop ERROR: failed to stop watcher", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DeviceDiscoveryService] Stop ERROR: failed to stop watcher", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceDiscoveryService] Stop ERROR: failed to stop watcher");
    }
    try {
        if (addedToken.value != 0) watcher.Added(addedToken);
        if (removedToken.value != 0) watcher.Removed(removedToken);
        if (enumerationCompletedToken.value != 0) watcher.EnumerationCompleted(enumerationCompletedToken);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DeviceDiscoveryService] Stop ERROR: failed to revoke watcher token", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DeviceDiscoveryService] Stop ERROR: failed to revoke watcher token", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceDiscoveryService] Stop ERROR: failed to revoke watcher token");
    }

    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherGeneration == stoppedGeneration) {
            m_watcherStopping = false;
        }
    }
}

void DeviceDiscoveryService::ClearCache() {
    bool changed = false;
    {
        auto guard = m_lock.lock_exclusive();
        changed = !m_deviceCache.empty() || m_enumerationComplete;
        m_deviceCache.clear();
        m_enumerationComplete = false;
        if (changed) ++m_inventoryGeneration;
    }
    if (changed) InventoryChanged();
}

bool DeviceDiscoveryService::ContainsDeviceId(std::wstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_deviceCache.contains(deviceId);
}

apc::device_picker::DeviceInventorySnapshot DeviceDiscoveryService::GetInventorySnapshot() const {
    auto guard = m_lock.lock_shared();
    return BuildInventorySnapshot(m_inventoryGeneration, m_enumerationComplete, m_deviceCache);
}

std::optional<apc::device_picker::DeviceInventorySnapshot>
DeviceDiscoveryService::GetInventorySnapshotIfChanged(std::uint64_t knownGeneration) const {
    auto guard = m_lock.lock_shared();
    if (m_inventoryGeneration == knownGeneration) return std::nullopt;
    return BuildInventorySnapshot(m_inventoryGeneration, m_enumerationComplete, m_deviceCache);
}

std::size_t DeviceDiscoveryService::CacheSize() const {
    auto guard = m_lock.lock_shared();
    return m_deviceCache.size();
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
DeviceDiscoveryService::RefreshAsync() {
    std::uint64_t inventoryGenerationAtStart = 0;
    {
        auto guard = m_lock.lock_shared();
        inventoryGenerationAtStart = m_inventoryGeneration;
    }
    auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
    auto devices = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);

    std::unordered_map<std::wstring, std::wstring> refreshed;
    refreshed.reserve(static_cast<size_t>(devices.Size()));
    for (auto const& device : devices) {
        refreshed.insert_or_assign(std::wstring(device.Id()), std::wstring(device.Name()));
    }

    bool inventoryUpdated = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_inventoryGeneration == inventoryGenerationAtStart) {
            inventoryUpdated = m_deviceCache != refreshed || !m_enumerationComplete;
            if (inventoryUpdated) {
                m_deviceCache = std::move(refreshed);
                m_enumerationComplete = true;
                ++m_inventoryGeneration;
            }
        }
    }
    if (inventoryUpdated) InventoryChanged();

    co_return devices;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceDiscoveryService::OnDeviceAdded(std::uint64_t watcherGeneration,
                                           winrt::Windows::Devices::Enumeration::DeviceWatcher const&,
                                           winrt::Windows::Devices::Enumeration::DeviceInformation const& args) {
    bool inventoryChanged = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherStopping || watcherGeneration != m_watcherGeneration) return;
        auto id = std::wstring(args.Id());
        auto name = std::wstring(args.Name());
        auto [entry, inserted] = m_deviceCache.try_emplace(std::move(id), name);
        inventoryChanged = inserted || entry->second != name;
        if (inventoryChanged) {
            entry->second = std::move(name);
            ++m_inventoryGeneration;
        }
    }
    DeviceAdded(args);
    if (inventoryChanged) InventoryChanged();
}

void DeviceDiscoveryService::OnDeviceRemoved(
    std::uint64_t watcherGeneration,
    winrt::Windows::Devices::Enumeration::DeviceWatcher const&,
    winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& args) {
    bool inventoryChanged = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherStopping || watcherGeneration != m_watcherGeneration) return;
        inventoryChanged = m_deviceCache.erase(std::wstring(args.Id())) > 0;
        if (inventoryChanged) ++m_inventoryGeneration;
    }
    DeviceRemoved(args);
    if (inventoryChanged) InventoryChanged();
}

void DeviceDiscoveryService::OnEnumerationCompleted(std::uint64_t watcherGeneration,
                                                    winrt::Windows::Devices::Enumeration::DeviceWatcher const&) {
    bool inventoryChanged = false;
    {
        auto guard = m_lock.lock_exclusive();
        if (m_watcherStopping || watcherGeneration != m_watcherGeneration) return;
        if (!m_enumerationComplete) {
            m_enumerationComplete = true;
            ++m_inventoryGeneration;
            inventoryChanged = true;
        }
    }
    if (inventoryChanged) InventoryChanged();
}
