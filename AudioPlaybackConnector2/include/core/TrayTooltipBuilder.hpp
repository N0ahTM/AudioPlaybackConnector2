#pragma once

#include <core/DeviceTrayPresentation.hpp>
#include <core/SettingsData.hpp>

#include <span>
#include <string>
#include <string_view>

namespace apc::tray {

[[nodiscard]] std::wstring BuildTooltip(std::wstring_view appName,
                                        std::wstring_view redactedDeviceName,
                                        std::span<const DeviceTrayPresentationItem> connectedDevices,
                                        std::span<const DeviceSettings> deviceSettings,
                                        bool privacyModeEnabled);

} // namespace apc::tray
