#pragma once

#include <unordered_set>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Discovery Service //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceDiscoveryService {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using DeviceAddedEvent = Event<winrt::Windows::Devices::Enumeration::DeviceInformation>;
    using DeviceRemovedEvent = Event<winrt::Windows::Devices::Enumeration::DeviceInformationUpdate>;

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
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshAsync();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Events ////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceAddedEvent DeviceAdded;
    DeviceRemovedEvent DeviceRemoved;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceWatcher const& sender,
                       winrt::Windows::Devices::Enumeration::DeviceInformation const& args);
    void OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceWatcher const& sender,
                         winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& args);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::unordered_set<std::wstring> m_deviceCache;
    winrt::Windows::Devices::Enumeration::DeviceWatcher m_watcher{nullptr};
    winrt::event_token m_watcherAddedToken{};
    winrt::event_token m_watcherRemovedToken{};
    bool m_watcherStopping = false;
};
