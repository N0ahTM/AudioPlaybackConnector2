#pragma once

#include <functional>
#include <memory>

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Notification Service //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

class NotificationService : public std::enable_shared_from_this<NotificationService> {
public:
    /* Notification Types */
    /*------------------------------------------------------------------------------------------------------------------*/

    enum class FallbackNotificationType { Info,
                                          Warning,
                                          Error };

    /* Callback Types */
    /*------------------------------------------------------------------------------------------------------------------*/

    using ReconnectRequestedCallback = std::function<void(winrt::hstring deviceId)>;
    using FallbackNotificationCallback = std::function<void(std::wstring const& title, std::wstring const& body, FallbackNotificationType type)>;

    /* Lifecycle */
    /*------------------------------------------------------------------------------------------------------------------*/

    NotificationService() = default;
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    [[nodiscard]] bool Initialize(winrt::hstring const& appName, winrt::Windows::Foundation::Uri const& logoUri);
    void Teardown() noexcept;

    /* Callbacks */
    /*------------------------------------------------------------------------------------------------------------------*/

    void SetReconnectCallback(ReconnectRequestedCallback callback);
    void SetFallbackNotifier(FallbackNotificationCallback callback);

    /* Notifications */
    /*------------------------------------------------------------------------------------------------------------------*/

    void ShowAppStarted();
    void ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowDeviceDisconnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnect(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName);

    void OnNotificationInvoked(winrt::Microsoft::Windows::AppNotifications::AppNotificationActivatedEventArgs const& args);

private:
    /* Internal Helpers */
    /*------------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool TryShowToast(std::wstring const& xml, winrt::hstring const& tag, winrt::Windows::Foundation::DateTime const& expiration);
    void ShowToastOrFallback(std::wstring const& xml,
                             winrt::hstring const& tag,
                             winrt::Windows::Foundation::DateTime const& expiration,
                             std::wstring const& fallbackTitle,
                             std::wstring const& fallbackBody,
                             FallbackNotificationType fallbackType);

    /* Member Variables */
    /*------------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::Windows::AppNotifications::AppNotificationManager m_notificationManager{nullptr};
    winrt::event_token m_notificationInvokedToken{};
    ReconnectRequestedCallback m_reconnectCallback;
    FallbackNotificationCallback m_fallbackNotifier;
    bool m_notificationsRegistered = false;
    bool m_isTearingDown = false;
    mutable wil::srwlock m_lock;
};
