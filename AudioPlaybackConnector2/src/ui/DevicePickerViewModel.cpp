#include <pch.h>

#include <ui/DevicePickerViewModel.hpp>

#include <core/DeviceDisplay.hpp>
#include <core/DeviceManager.hpp>
#include <core/Settings.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerViewModel::SetDeviceManager(std::weak_ptr<DeviceManager> manager) {
    m_manager = std::move(manager);
}

void DevicePickerViewModel::SetSettings(std::weak_ptr<Settings> settings) {
    m_settings = std::move(settings);
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
    SettingsData settingsSnapshot;
    bool hasSettings = false;
    if (auto settings = m_settings.lock()) {
        auto locked = settings->LockSharedData();
        settingsSnapshot = *locked;
        hasSettings = true;
    }

    std::vector<DevicePickerItemViewModel> items;
    items.reserve(m_devices.size());
    for (auto const& device : m_devices) {
        auto deviceId = std::wstring(device.Id);
        auto displayName = std::wstring(device.Name);
        auto it = settingsSnapshot.Devices.end();
        if (hasSettings) {
            it = std::ranges::find_if(settingsSnapshot.Devices,
                                      [&](auto const& knownDevice) { return knownDevice.Id == deviceId; });
            if (it != settingsSnapshot.Devices.end()) {
                auto knownName = it->Name.empty() ? std::wstring(device.Name) : it->Name;
                displayName =
                    apc::display::DeviceNameOrId(it->Id, knownName, it->Alias, settingsSnapshot.PrivacyModeEnabled);
            } else if (settingsSnapshot.PrivacyModeEnabled) {
                displayName = apc::display::DeviceNameOrId(deviceId, device.Name, {}, true);
            }
        }
        if (displayName.empty()) {
            displayName = deviceId;
        }

        items.push_back({
            .Id = device.Id,
            .Name = winrt::hstring(displayName),
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
