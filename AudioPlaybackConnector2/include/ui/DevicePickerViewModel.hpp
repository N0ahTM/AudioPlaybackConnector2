#pragma once

#include <memory>
#include <vector>

class DeviceManager;

struct CachedDevicePickerDevice {
    winrt::hstring Id;
    winrt::hstring Name;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Picker View Model //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct DevicePickerItemViewModel {
    winrt::hstring Id;
    winrt::hstring Name;
    bool IsConnected = false;
    bool IsBusy = false;
};

class DevicePickerViewModel {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetDeviceManager(std::weak_ptr<DeviceManager> manager);
    void SetDevices(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices);
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::vector<DevicePickerItemViewModel> SnapshotItems() const;
    [[nodiscard]] bool CanSelect(winrt::hstring const& id) const;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool IsConnected(winrt::hstring const& id) const;
    [[nodiscard]] bool IsBusy(winrt::hstring const& id) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::weak_ptr<DeviceManager> m_manager;
    std::vector<CachedDevicePickerDevice> m_devices;
};
