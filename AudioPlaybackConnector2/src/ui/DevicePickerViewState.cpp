#include <ui/DevicePickerViewState.hpp>
#include <algorithm>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Snapshot Projection ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DevicePickerViewState BuildDevicePickerViewState(apc::app::AppSnapshot const& snapshot,
                                                 std::wstring_view privateDeviceName) {
    DevicePickerViewState result;
    result.Generation = snapshot.Generation;
    result.InventoryComplete = snapshot.InventoryComplete;
    if (!snapshot.IsRunning) return result;
    for (auto const& device : snapshot.Devices) {
        auto saved = std::ranges::find(snapshot.Settings.Devices, device.Id.View(), &DeviceSettings::Id);
        if (!device.IsAvailable && saved == snapshot.Settings.Devices.end()) continue;
        auto name = device.Name.empty() ? device.Id.ToString() : device.Name;
        result.Items.push_back({device.Id.ToString(),
                                name,
                                device.Alias,
                                !device.Alias.empty()         ? device.Alias
                                : snapshot.PrivacyModeEnabled ? std::wstring(privateDeviceName)
                                                              : name,
                                device.IsConnected,
                                device.IsBusy,
                                device.IsAvailable,
                                snapshot.Settings.DefaultDevice == DefaultDeviceMode::SpecificDevice &&
                                    snapshot.Settings.DefaultDeviceId == device.Id.View()});
        if (device.IsConnected) ++result.ConnectedDeviceCount;
    }
    // A persisted default may outlive discovery and its saved device row; keep its reset action reachable.
    auto const& settings = snapshot.Settings;
    if (settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && !settings.DefaultDeviceId.empty() &&
        std::ranges::none_of(result.Items, [&](auto const& item) { return item.Id == settings.DefaultDeviceId; })) {
        auto const& id = settings.DefaultDeviceId;
        result.Items.push_back({id,
                                id,
                                {},
                                snapshot.PrivacyModeEnabled ? std::wstring(privateDeviceName) : id,
                                false,
                                false,
                                false,
                                true});
    }
    return result;
}

std::optional<DeviceOptionsViewModel> BuildDeviceOptionsViewState(apc::app::AppSnapshot const& snapshot,
                                                                  std::wstring_view id,
                                                                  std::wstring_view privateDeviceName) {
    if (!snapshot.IsRunning || id.empty()) return std::nullopt;
    auto device = std::ranges::find_if(snapshot.Devices, [&](auto const& value) { return value.Id.View() == id; });
    if (device == snapshot.Devices.end()) {
        if (snapshot.Settings.DefaultDevice != DefaultDeviceMode::SpecificDevice ||
            snapshot.Settings.DefaultDeviceId != id)
            return std::nullopt;
        DeviceOptionsViewModel result;
        result.Device.Id = id;
        result.Device.DisplayName = snapshot.PrivacyModeEnabled ? std::wstring(privateDeviceName) : std::wstring(id);
        result.Device.IsDefaultDevice = true;
        result.GlobalConnectOnStartup = snapshot.Settings.GlobalConnectOnStartup;
        result.GlobalReconnectOnConnectionLoss = snapshot.Settings.GlobalReconnectOnConnectionLoss;
        return result;
    }
    auto const& settings = snapshot.Settings;
    auto saved = std::ranges::find(settings.Devices, id, &DeviceSettings::Id);
    if (!device->IsAvailable && saved == settings.Devices.end() &&
        !(settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == id))
        return std::nullopt;
    DeviceOptionsViewModel result;
    result.Device.Id = id;
    result.Device.Alias = device->Alias;
    result.Device.DisplayName = !device->Alias.empty()        ? device->Alias
                                : snapshot.PrivacyModeEnabled ? std::wstring(privateDeviceName)
                                : device->Name.empty()        ? device->Id.ToString()
                                                              : device->Name;
    result.Device.IsDefaultDevice =
        settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == id;
    if (saved != settings.Devices.end()) {
        result.Device.ConnectOnStartup = saved->ConnectOnStartup;
        result.Device.ReconnectOnConnectionLoss = saved->ReconnectOnConnectionLoss;
        result.CanForget = !device->IsConnected && !device->IsBusy;
    }
    result.GlobalConnectOnStartup = settings.GlobalConnectOnStartup;
    result.GlobalReconnectOnConnectionLoss = settings.GlobalReconnectOnConnectionLoss;
    return result;
}
