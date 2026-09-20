#include <pch.h>

#include <core/AudioConnectionService.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::Windows::Media::Audio::AudioPlaybackConnection
AudioConnectionService::TryCreateFromId(winrt::hstring const& deviceId) {
    return winrt::Windows::Media::Audio::AudioPlaybackConnection::TryCreateFromId(deviceId);
}

winrt::event_token
AudioConnectionService::RegisterStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection,
                                             StateChangedHandler handler) {
    if (!connection) return {};
    return connection.StateChanged([handler = std::move(handler)](auto sender, auto args) { handler(sender, args); });
}

void AudioConnectionService::RevokeStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection,
                                                winrt::event_token token) noexcept {
    if (!connection || token.value == 0) return;
    try {
        connection.StateChanged(token);
    } catch (...) {
    }
}

winrt::Windows::Foundation::IAsyncAction
AudioConnectionService::StartAsync(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection) {
    return connection.StartAsync();
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResult>
AudioConnectionService::OpenAsync(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection) {
    return connection.OpenAsync();
}

void AudioConnectionService::Close(winrt::Windows::Media::Audio::AudioPlaybackConnection& connection) noexcept {
    if (!connection) return;
    try {
        connection.Close();
    } catch (...) {
    }
    connection = nullptr;
}
