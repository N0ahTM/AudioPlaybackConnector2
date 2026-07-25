#include <pch.h>
#include <core/DeviceManagerDiagnostics.hpp>

std::wstring_view DeviceConnectionStateName(winrt::Windows::Media::Audio::AudioPlaybackConnectionState state) noexcept {
    switch (state) {
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed: return L"Closed";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Opened: return L"Opened";
        default: return L"UnknownState";
    }
}

std::wstring_view
DeviceOpenResultStatusName(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus status) noexcept {
    switch (status) {
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::Success: return L"Success";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
            return L"RequestTimedOut";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
            return L"DeniedBySystem";
        case winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
            return L"UnknownFailure";
        default: return L"UnknownStatus";
    }
}

void LogDeviceManagerDiagnosticSnapshot(winrt::hstring const& reason,
                                        std::size_t deviceCacheSize,
                                        bool allReconnectsCancelled,
                                        std::vector<DeviceManagerDiagnosticSnapshot> const& snapshots) {
    DebugTrace(L"[DeviceManager] Snapshot reason={0} connections={1} cache={2} allReconnectsCancelled={3}",
               std::wstring(reason),
               snapshots.size(),
               deviceCacheSize,
               allReconnectsCancelled);

    for (auto const& snapshot : snapshots) {
        std::wstring state = L"<null>";
        if (snapshot.HasConnection) {
            state = snapshot.IsOpen ? L"Opened(cached)" : L"Closed(cached)";
        }

        DebugTrace(
            L"[DeviceManager] Snapshot device id={0} name={1} isOpen={2} state={3} reconnectOnLoss={4} "
            L"acceptIncoming={5} disconnecting={6} reconnecting={7} cancelledReconnect={8} reconnectAttempts={9} "
            L"connectAttemptId={10}",
            std::wstring(snapshot.Id),
            std::wstring(snapshot.Name),
            snapshot.IsOpen,
            state,
            snapshot.ReconnectOnConnectionLoss,
            snapshot.AcceptIncomingConnections,
            snapshot.Disconnecting,
            snapshot.Reconnecting,
            snapshot.CancelledReconnect,
            snapshot.ReconnectAttempts,
            snapshot.ConnectAttemptId);
    }
}
