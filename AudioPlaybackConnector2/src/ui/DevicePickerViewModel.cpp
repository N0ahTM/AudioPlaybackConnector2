#include <pch.h>

#include <ui/DevicePickerViewModel.hpp>

#include <core/DeviceService.hpp>
#include <core/SettingsStore.hpp>
#include <core/StringResources.hpp>
#include <services/SettingsController.hpp>
#include <ui/SettingsViewModel.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerViewModel::SetDeviceService(std::weak_ptr<apc::device::DeviceService> service) {
    m_service = std::move(service);
}

void DevicePickerViewModel::SetSettingsStore(std::weak_ptr<SettingsStore> settingsStore) {
    m_settingsStore = std::move(settingsStore);
}

void DevicePickerViewModel::SetDeviceSettings(std::weak_ptr<ISettingsController> controller,
                                              apc::app::SettingsWindowCommandExecutor::ExecuteCallback execute) {
    m_settingsController = std::move(controller);
    m_deviceCommands.emplace(std::move(execute));
}

std::optional<DeviceOptionsViewModel> DevicePickerViewModel::DeviceOptions(std::wstring_view id) const {
    auto controller = m_settingsController.lock();
    if (!controller || id.empty()) return std::nullopt;
    const auto settings = controller->Snapshot();
    auto devices = SettingsViewModel::BuildDeviceItems(settings);
    auto const saved = std::ranges::find(devices, id, &SettingsDeviceViewModel::Id);
    auto const& items = m_cache.CachedSnapshot().Items;
    auto const discovered = std::ranges::find(items, id, &apc::device_picker::DeviceSnapshotItem::Id);
    if (saved == devices.end() && discovered == items.end()) return std::nullopt;

    DeviceOptionsViewModel result;
    if (saved != devices.end())
        result.Device = *saved;
    else {
        result.Device.Id = discovered->Id;
        result.Device.DisplayName = discovered->DisplayName;
        result.Device.IsDefaultDevice =
            settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == id;
    }
    result.GlobalConnectOnStartup = settings.GlobalConnectOnStartup;
    result.GlobalReconnectOnConnectionLoss = settings.GlobalReconnectOnConnectionLoss;
    if (auto service = m_service.lock()) {
        const auto activity = service->GetDevicePickerActivitySnapshot();
        result.CanForget = saved != devices.end() && !activity.ConnectedIds.contains(result.Device.Id) &&
                           !activity.BusyIds.contains(result.Device.Id);
    }
    return result;
}

bool DevicePickerViewModel::SetAlias(std::wstring_view id, std::wstring_view alias) const {
    return m_deviceCommands && m_deviceCommands->SetAlias(id, alias).Succeeded();
}

bool DevicePickerViewModel::SetDefault(std::wstring_view id, bool enabled) const {
    if (!m_deviceCommands) return false;
    if (!enabled) {
        const auto options = DeviceOptions(id);
        if (!options) return false;
        if (!options->Device.IsDefaultDevice) return true;
    }
    return (enabled ? m_deviceCommands->SetDefault(id) : m_deviceCommands->ClearDefault()).Succeeded();
}

bool DevicePickerViewModel::SetConnectOnStartup(std::wstring const& id, bool enabled) const {
    auto controller = m_settingsController.lock();
    if (!controller) return false;
    controller->SetDeviceConnectOnStartup(id, enabled);
    const auto options = DeviceOptions(id);
    return options && options->Device.ConnectOnStartup == enabled;
}

bool DevicePickerViewModel::SetReconnectOnConnectionLoss(std::wstring const& id, bool enabled) const {
    auto controller = m_settingsController.lock();
    if (!controller) return false;
    controller->SetDeviceReconnectOnConnectionLoss(id, enabled);
    const auto options = DeviceOptions(id);
    return options && options->Device.ReconnectOnConnectionLoss == enabled;
}

bool DevicePickerViewModel::ForgetDevice(std::wstring const& id) const {
    auto controller = m_settingsController.lock();
    const auto options = DeviceOptions(id);
    if (!controller || !options || !options->CanForget) return false;
    controller->ForgetDevice(id);
    const auto settings = controller->Snapshot();
    return std::ranges::find(settings.Devices, id, &DeviceSettings::Id) == settings.Devices.end();
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

bool DevicePickerViewModel::SynchronizeInventoryFromService(TimePoint refreshedAt) {
    auto service = m_service.lock();
    if (!service) return false;

    if (m_sourceInventoryGeneration && m_cache.HasInventory()) {
        auto inventory = service->GetDevicePickerInventorySnapshotIfChanged(*m_sourceInventoryGeneration);
        if (!inventory) return m_sourceEnumerationComplete;

        m_cache.ReplaceInventory(std::move(inventory->Devices), refreshedAt);
        m_sourceInventoryGeneration = inventory->Generation;
        m_sourceEnumerationComplete = inventory->EnumerationComplete;
    } else {
        auto inventory = service->GetDevicePickerInventorySnapshot();
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
    if (auto service = m_service.lock()) {
        activity = service->GetDevicePickerActivitySnapshot();
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
                .IsDefault = snapshot.Data.DefaultDevice == DefaultDeviceMode::SpecificDevice &&
                             snapshot.Data.DefaultDeviceId == device.Id,
            });
        }
        if (snapshot.Data.DefaultDevice == DefaultDeviceMode::SpecificDevice &&
            !snapshot.Data.DefaultDeviceId.empty() &&
            std::ranges::none_of(presentationSettings,
                                 [&](auto const& device) { return device.Id == snapshot.Data.DefaultDeviceId; })) {
            presentationSettings.push_back({snapshot.Data.DefaultDeviceId, {}, {}, true});
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
