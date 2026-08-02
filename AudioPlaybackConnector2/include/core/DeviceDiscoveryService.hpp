#pragma once

#include <core/DevicePickerSnapshot.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Discovery Service //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceDiscoveryService : public std::enable_shared_from_this<DeviceDiscoveryService> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using DeviceAddedEvent = Event<winrt::Windows::Devices::Enumeration::DeviceInformation>;
    using DeviceRemovedEvent = Event<winrt::Windows::Devices::Enumeration::DeviceInformationUpdate>;
    using InventoryChangedEvent = Event<>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    ~DeviceDiscoveryService();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Start();
    void Stop();
    void ClearCache();
    [[nodiscard]] bool ContainsDeviceId(std::wstring const& deviceId) const;
    [[nodiscard]] std::size_t CacheSize() const;
    [[nodiscard]] apc::device_picker::DeviceInventorySnapshot GetInventorySnapshot() const;
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshAsync();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Events ////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceAddedEvent DeviceAdded;
    DeviceRemovedEvent DeviceRemoved;
    InventoryChangedEvent InventoryChanged;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void OnDeviceAdded(std::uint64_t watcherGeneration,
                       winrt::Windows::Devices::Enumeration::DeviceWatcher const& sender,
                       winrt::Windows::Devices::Enumeration::DeviceInformation const& args);
    void OnDeviceRemoved(std::uint64_t watcherGeneration,
                         winrt::Windows::Devices::Enumeration::DeviceWatcher const& sender,
                         winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& args);
    void OnEnumerationCompleted(std::uint64_t watcherGeneration,
                                winrt::Windows::Devices::Enumeration::DeviceWatcher const& sender);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::mutex m_watcherLifecycleMutex;
    std::unordered_map<std::wstring, std::wstring> m_deviceCache;
    winrt::Windows::Devices::Enumeration::DeviceWatcher m_watcher{nullptr};
    winrt::event_token m_watcherAddedToken{};
    winrt::event_token m_watcherRemovedToken{};
    winrt::event_token m_watcherEnumerationCompletedToken{};
    std::uint64_t m_watcherGeneration = 0;
    std::uint64_t m_inventoryGeneration = 0;
    bool m_enumerationComplete = false;
    bool m_watcherStopping = false;
};
