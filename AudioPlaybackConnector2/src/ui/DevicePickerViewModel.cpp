#include <pch.h>

#include <ui/DevicePickerViewModel.hpp>

#include <core/DeviceManager.hpp>
#include <core/SettingsStore.hpp>
#include <core/StringResources.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerViewModel::SetDeviceManager(std::weak_ptr<DeviceManager> manager) {
    m_manager = std::move(manager);
}

void DevicePickerViewModel::SetSettingsStore(std::weak_ptr<SettingsStore> settingsStore) {
    m_settingsStore = std::move(settingsStore);
}

void DevicePickerViewModel::SetDevices(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
                                       TimePoint refreshedAt) {
    std::vector<apc::device_picker::DeviceIdentity> inventory;
    if (devices) {
        inventory.reserve(devices.Size());
        for (auto const& device : devices) {
            inventory.push_back({
                .Id = std::wstring(device.Id()),
                .Name = std::wstring(device.Name()),
            });
        }
    }
    m_cache.ReplaceInventory(std::move(inventory), refreshedAt);
}

bool DevicePickerViewModel::SynchronizeInventoryFromManager(TimePoint refreshedAt) {
    auto manager = m_manager.lock();
    if (!manager) return false;

    if (m_sourceInventoryGeneration && m_cache.HasInventory()) {
        auto inventory = manager->GetDevicePickerInventorySnapshotIfChanged(*m_sourceInventoryGeneration);
        if (!inventory) return m_sourceEnumerationComplete;

        m_cache.ReplaceInventory(std::move(inventory->Devices), refreshedAt);
        m_sourceInventoryGeneration = inventory->Generation;
        m_sourceEnumerationComplete = inventory->EnumerationComplete;
    } else {
        auto inventory = manager->GetDevicePickerInventorySnapshot();
        m_cache.ReplaceInventory(std::move(inventory.Devices), refreshedAt);
        m_sourceInventoryGeneration = inventory.Generation;
        m_sourceEnumerationComplete = inventory.EnumerationComplete;
    }
    if (!m_sourceEnumerationComplete) m_cache.InvalidateInventory();
    return m_sourceEnumerationComplete;
}

void DevicePickerViewModel::InvalidateInventory() noexcept {
    m_cache.InvalidateInventory();
}

void DevicePickerViewModel::Clear() noexcept {
    m_cache.Clear();
    m_sourceInventoryGeneration.reset();
    m_sourceEnumerationComplete = false;
}

bool DevicePickerViewModel::HasInventory() const noexcept {
    return m_cache.HasInventory();
}

bool DevicePickerViewModel::IsInventoryFresh(TimePoint now) const noexcept {
    return m_cache.IsInventoryFresh(now);
}

apc::device_picker::DevicePickerSnapshot const& DevicePickerViewModel::RefreshSnapshot(TimePoint now) {
    apc::device_picker::DeviceActivitySnapshot activity;
    if (auto manager = m_manager.lock()) {
        activity = manager->GetDevicePickerActivitySnapshot();
    }

    std::vector<apc::device_picker::DevicePresentationSetting> presentationSettings;
    bool privacyModeEnabled = false;
    if (auto settingsStore = m_settingsStore.lock()) {
        const auto snapshot = settingsStore->Snapshot();
        privacyModeEnabled = snapshot.Data.PrivacyModeEnabled;
        presentationSettings.reserve(snapshot.Data.Devices.size());
        for (auto const& device : snapshot.Data.Devices) {
            presentationSettings.push_back({
                .Id = device.Id,
                .Name = device.Name,
                .Alias = device.Alias,
            });
        }
    }

    return m_cache.Refresh(
        activity, presentationSettings, privacyModeEnabled, std::wstring_view(_("Privacy_RedactedDevice")), now);
}

apc::device_picker::DevicePickerSnapshot const& DevicePickerViewModel::CachedSnapshot() const noexcept {
    return m_cache.CachedSnapshot();
}

bool DevicePickerViewModel::CanSelect(winrt::hstring const& id) const noexcept {
    if (id.empty()) return false;
    return m_cache.CanSelect(std::wstring_view(id));
}
