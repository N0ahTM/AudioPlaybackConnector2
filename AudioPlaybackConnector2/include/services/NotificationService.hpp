#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Notification Service //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class NotificationService : public std::enable_shared_from_this<NotificationService> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Notification Types ////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    enum class FallbackNotificationType { Info, Warning, Error };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callback Types ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ReconnectRequestedCallback = std::function<void(winrt::hstring deviceId)>;
    using FallbackNotificationCallback =
        std::function<void(std::wstring const& title, std::wstring const& body, FallbackNotificationType type)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    NotificationService() = default;
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    [[nodiscard]] bool Initialize(winrt::hstring const& appName, winrt::Windows::Foundation::Uri const& logoUri);
    void Teardown() noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetReconnectCallback(ReconnectRequestedCallback callback);
    void SetFallbackNotifier(FallbackNotificationCallback callback);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Notifications /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowAppStarted();
    void ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowDeviceDisconnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnect(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowUpdateAvailable(std::wstring const& latestVersion);

    void
    OnNotificationInvoked(winrt::Microsoft::Windows::AppNotifications::AppNotificationActivatedEventArgs const& args);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct StatusNotificationTagReservation {
        std::vector<winrt::hstring> TagsToRemove;
        winrt::hstring CurrentTag;
        uint64_t Generation = 0;
    };

    [[nodiscard]] StatusNotificationTagReservation ReserveStatusNotificationTag();
    [[nodiscard]] bool IsStatusNotificationGenerationCurrent(uint64_t generation) const;
    // NOTE: This is a coroutine. All parameters are passed by value intentionally to ensure
    // they remain valid across suspension points. Do NOT change to const&.
    winrt::fire_and_forget ShowToastOrFallbackAsync(std::wstring xml,
                                                    winrt::hstring group,
                                                    winrt::hstring tag,
                                                    std::vector<winrt::hstring> tagsToRemove,
                                                    uint64_t generation,
                                                    winrt::Windows::Foundation::DateTime expiration,
                                                    std::wstring fallbackTitle,
                                                    std::wstring fallbackBody,
                                                    FallbackNotificationType fallbackType);
    void ShowStatusToastOrFallback(std::wstring const& xml,
                                   winrt::Windows::Foundation::DateTime const& expiration,
                                   std::wstring const& fallbackTitle,
                                   std::wstring const& fallbackBody,
                                   FallbackNotificationType fallbackType);
    void ShowFallbackNotification(std::wstring const& fallbackTitle,
                                  std::wstring const& fallbackBody,
                                  FallbackNotificationType fallbackType);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::Windows::AppNotifications::AppNotificationManager m_notificationManager{nullptr};
    winrt::event_token m_notificationInvokedToken{};
    ReconnectRequestedCallback m_reconnectCallback;
    FallbackNotificationCallback m_fallbackNotifier;
    std::vector<winrt::hstring> m_statusNotificationTags;
    uint64_t m_statusNotificationGeneration = 0;
    bool m_notificationsRegistered = false;
    bool m_isTearingDown = false;
    mutable wil::srwlock m_lock;
};
