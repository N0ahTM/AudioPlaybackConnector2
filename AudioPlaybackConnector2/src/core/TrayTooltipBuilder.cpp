#include <core/TrayTooltipBuilder.hpp>

#include <algorithm>

namespace apc::tray {

std::wstring BuildTooltip(std::wstring_view appName,
                          std::wstring_view redactedDeviceName,
                          std::span<const DeviceTrayPresentationItem> connectedDevices,
                          std::span<const DeviceSettings> deviceSettings,
                          bool privacyModeEnabled) {
    if (connectedDevices.empty()) return std::wstring(appName);

    std::wstring tooltip(appName);
    tooltip += L'\n';
    for (auto const& connected : connectedDevices) {
        auto const persisted = std::ranges::find_if(
            deviceSettings, [&](DeviceSettings const& device) { return device.Id == connected.Id; });

        std::wstring_view alias;
        std::wstring_view name = connected.Name;
        if (persisted != deviceSettings.end()) {
            alias = persisted->Alias;
            if (!persisted->Name.empty()) name = persisted->Name;
        }

        if (!alias.empty()) {
            tooltip += alias;
        } else if (privacyModeEnabled) {
            tooltip += redactedDeviceName;
        } else if (!name.empty()) {
            tooltip += name;
        } else {
            tooltip += connected.Id;
        }
        tooltip += L'\n';
    }
    return tooltip;
}

} // namespace apc::tray
