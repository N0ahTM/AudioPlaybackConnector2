#pragma once

#include <core/Settings.hpp>
#include <core/StringResources.hpp>

namespace apc::display {

inline std::wstring
DeviceName(std::wstring_view id, std::wstring_view name, std::wstring_view alias, bool privacyModeEnabled) {
    if (!alias.empty()) return std::wstring(alias);
    if (privacyModeEnabled) return std::wstring(_("Privacy_RedactedDevice"));
    if (!name.empty()) return std::wstring(name);
    return std::wstring(id);
}

inline std::wstring DeviceName(DeviceSettings const& device, bool privacyModeEnabled) {
    return DeviceName(device.Id, device.Name, device.Alias, privacyModeEnabled);
}

inline std::wstring
DeviceNameOrId(std::wstring_view id, std::wstring_view name, std::wstring_view alias, bool privacyModeEnabled) {
    auto displayName = DeviceName(id, name, alias, privacyModeEnabled);
    if (!displayName.empty()) return displayName;
    return std::wstring(id);
}

} // namespace apc::display
