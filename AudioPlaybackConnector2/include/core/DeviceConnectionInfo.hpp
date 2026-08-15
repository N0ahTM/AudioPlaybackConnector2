#pragma once

#include <string>

struct DeviceConnectionInfo {
    // Plain-string metadata captured on the connect thread; never touch WinRT DeviceInformation from UI.
    std::wstring Id;
    std::wstring Name;
    winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
    winrt::event_token StateChangedToken{};
    bool ReconnectOnConnectionLoss = false;
    bool AcceptIncomingConnections = false;
    bool IsOpen = false;
};
