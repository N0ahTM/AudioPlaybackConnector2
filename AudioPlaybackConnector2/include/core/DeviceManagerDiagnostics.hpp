#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

struct DeviceManagerDiagnosticSnapshot {
    winrt::hstring Id;
    winrt::hstring Name;
    bool HasConnection = false;
    bool IsOpen = false;
    bool ReconnectOnConnectionLoss = false;
    bool AcceptIncomingConnections = false;
    bool Disconnecting = false;
    bool Reconnecting = false;
    bool CancelledReconnect = false;
    std::size_t ReconnectAttempts = 0;
    std::size_t ConnectAttemptId = 0;
};

std::wstring_view DeviceConnectionStateName(winrt::Windows::Media::Audio::AudioPlaybackConnectionState state) noexcept;
std::wstring_view
DeviceOpenResultStatusName(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus status) noexcept;
void LogDeviceManagerDiagnosticSnapshot(winrt::hstring const& reason,
                                        std::size_t deviceCacheSize,
                                        bool allReconnectsCancelled,
                                        std::vector<DeviceManagerDiagnosticSnapshot> const& snapshots);
