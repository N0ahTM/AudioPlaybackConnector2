#pragma once

#include <functional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Audio Connection Service //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class AudioConnectionService {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using StateChangedHandler = std::function<void(winrt::Windows::Media::Audio::AudioPlaybackConnection,
                                                   winrt::Windows::Foundation::IInspectable)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] static winrt::Windows::Media::Audio::AudioPlaybackConnection
    TryCreateFromId(winrt::hstring const& deviceId);
    [[nodiscard]] static winrt::event_token
    RegisterStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection,
                         StateChangedHandler handler);
    static void RevokeStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection,
                                   winrt::event_token token) noexcept;
    [[nodiscard]] static winrt::Windows::Foundation::IAsyncAction
    StartAsync(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection);
    [[nodiscard]] static winrt::Windows::Foundation::IAsyncOperation<
        winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResult>
    OpenAsync(winrt::Windows::Media::Audio::AudioPlaybackConnection const& connection);
    static void Close(winrt::Windows::Media::Audio::AudioPlaybackConnection& connection) noexcept;
    // Intentionally leaks the ABI reference to avoid STA re-entrancy crashes during process exit.
    // Must ONLY be called from shutdown paths (e.g. ShutdownForProcessExit).
    static void DetachForProcessExit(winrt::Windows::Media::Audio::AudioPlaybackConnection& connection) noexcept;
};
