#include <pch.h>

#include <ui/DevicePickerViewModel.hpp>

#include <core/DeviceManager.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerViewModel::SetDeviceManager(std::weak_ptr<DeviceManager> manager) {
    m_manager = std::move(manager);
}

void DevicePickerViewModel::SetDevices(
    winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices) {
    m_devices.clear();
    m_devices.reserve(devices.Size());
    for (auto const& device : devices) {
        auto id = device.Id();
        auto name = device.Name();
        if (name.empty()) {
            name = id;
        }
        m_devices.push_back({
            .Id = id,
            .Name = name,
        });
    }
}

bool DevicePickerViewModel::Empty() const noexcept {
    return m_devices.empty();
}

std::vector<DevicePickerItemViewModel> DevicePickerViewModel::SnapshotItems() const {
    std::vector<DevicePickerItemViewModel> items;
    items.reserve(m_devices.size());
    for (auto const& device : m_devices) {
        items.push_back({
            .Id = device.Id,
            .Name = device.Name,
            .IsConnected = IsConnected(device.Id),
            .IsBusy = IsBusy(device.Id),
        });
    }
    return items;
}

bool DevicePickerViewModel::CanSelect(winrt::hstring const& id) const {
    if (id.empty()) return false;
    if (IsBusy(id)) return false;
    return !IsConnected(id);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool DevicePickerViewModel::IsConnected(winrt::hstring const& id) const {
    auto manager = m_manager.lock();
    if (!manager) return false;

    for (const auto& connection : manager->GetConnectedDevices()) {
        if (connection.Id == std::wstring(id)) {
            return true;
        }
    }
    return false;
}

bool DevicePickerViewModel::IsBusy(winrt::hstring const& id) const {
    auto manager = m_manager.lock();
    return manager && manager->IsDeviceBusy(id);
}
