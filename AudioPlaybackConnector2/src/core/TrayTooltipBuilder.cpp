#include <core/TrayTooltipBuilder.hpp>

namespace apc::tray {

std::wstring
BuildTooltip(std::wstring_view appName, std::wstring_view redactedDeviceName, apc::app::AppSnapshot const& snapshot) {
    std::wstring tooltip(appName);
    if (snapshot.Tray.ConnectedDevices.empty()) return tooltip;
    tooltip += L'\n';
    for (auto const& device : snapshot.Tray.ConnectedDevices) {
        // DisplayName already resolves alias, saved name, live name and ID.
        tooltip += snapshot.PrivacyModeEnabled && device.Alias.empty() ? redactedDeviceName : device.DisplayName;
        tooltip += L'\n';
    }
    return tooltip;
}

} // namespace apc::tray
