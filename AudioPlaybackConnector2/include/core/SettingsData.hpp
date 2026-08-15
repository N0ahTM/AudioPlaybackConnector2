#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct DeviceSettings {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    bool ConnectOnStartup = false;
    bool ReconnectOnConnectionLoss = false;

    bool operator==(DeviceSettings const&) const = default;
};

enum class DefaultDeviceMode { LastConnected, SpecificDevice };

struct PersistedWindowBounds {
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Width = 0;
    int32_t Height = 0;
    uint32_t Dpi = USER_DEFAULT_SCREEN_DPI;

    bool operator==(PersistedWindowBounds const&) const = default;
};

struct SettingsData {
    bool GlobalConnectOnStartup = false;
    bool GlobalReconnectOnConnectionLoss = false;
    bool AllowIncomingConnections = false;
    bool StartWithWindows = false;
    bool ShowNotifications = true;
    bool UseSystemBackdropEffects = true;
    std::wstring Language = L"system";
    int64_t LastUpdateCheckUnixSeconds = 0;
    std::wstring LastNotifiedUpdateVersion;
    std::optional<PersistedWindowBounds> SettingsWindowBounds;
    bool PrivacyModeEnabled = false;
    DefaultDeviceMode DefaultDevice = DefaultDeviceMode::LastConnected;
    std::wstring DefaultDeviceId;
    std::vector<DeviceSettings> Devices;
    std::vector<std::wstring> LastConnectedIds;

    bool operator==(SettingsData const&) const = default;
};
