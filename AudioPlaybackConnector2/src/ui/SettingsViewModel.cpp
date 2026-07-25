#include <pch.h>

#include <ui/SettingsViewModel.hpp>

#include <core/DeviceDisplay.hpp>
#include <core/StringResources.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::vector<SettingsDeviceViewModel> SettingsViewModel::BuildDeviceItems(SettingsData const& settings) {
    std::vector<SettingsDeviceViewModel> items;
    items.reserve(settings.Devices.size());
    for (auto const& device : settings.Devices) {
        auto displayName =
            apc::display::DeviceNameOrId(device.Id, device.Name, device.Alias, settings.PrivacyModeEnabled);
        items.push_back({
            .Id = device.Id,
            .Name = device.Name,
            .Alias = device.Alias,
            .DisplayName = std::move(displayName),
            .ConnectOnStartup = device.ConnectOnStartup,
            .ReconnectOnConnectionLoss = device.ReconnectOnConnectionLoss,
            .IsDefaultDevice =
                settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == device.Id,
        });
    }
    return items;
}
