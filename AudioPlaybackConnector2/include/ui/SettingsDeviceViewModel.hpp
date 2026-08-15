#pragma once

#include <string>

struct SettingsDeviceViewModel {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    bool ConnectOnStartup = false;
    bool ReconnectOnConnectionLoss = false;
    bool IsDefaultDevice = false;
};
