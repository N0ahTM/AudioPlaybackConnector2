#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Notification Service //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class NotificationService : public std::enable_shared_from_this<NotificationService> {
public:
    using ReconnectRequestedCallback = std::function<void(winrt::hstring deviceId)>;
    using ShouldShowNotificationCallback = std::function<bool()>;

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
    void SetShouldShowNotificationCallback(ShouldShowNotificationCallback callback);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Notifications /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowAppStarted();
    void ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowDeviceDisconnected(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnect(winrt::hstring const& id, winrt::hstring const& deviceName);
    void ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName);
    [[nodiscard]] bool ShowUpdateAvailable(std::wstring const& latestVersion);

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

    void TeardownCore(bool clearCallbacks);
    [[nodiscard]] StatusNotificationTagReservation ReserveStatusNotificationTag();
    void RollbackStatusNotificationTag(StatusNotificationTagReservation&& reservation);
    [[nodiscard]] bool ShouldShowNotifications() const;
    winrt::fire_and_forget RemoveStaleStatusToastsAsync(
        winrt::Microsoft::Windows::AppNotifications::AppNotificationManager notificationManager,
        winrt::hstring group,
        std::vector<winrt::hstring> tagsToRemove);
    bool ShowStatusToast(std::wstring const& xml, winrt::Windows::Foundation::DateTime const& expiration);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::Windows::AppNotifications::AppNotificationManager m_notificationManager{nullptr};
    winrt::event_token m_notificationInvokedToken{};
    ReconnectRequestedCallback m_reconnectCallback;
    ShouldShowNotificationCallback m_shouldShowNotificationCallback;
    std::vector<winrt::hstring> m_statusNotificationTags;
    uint64_t m_statusNotificationGeneration = 0;
    bool m_notificationsRegistered = false;
    bool m_isTearingDown = false;
    std::mutex m_lifecycleMutex;
    std::mutex m_statusNotificationMutex;
    mutable wil::srwlock m_lock;
};
