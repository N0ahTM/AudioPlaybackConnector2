#pragma once

#include <core/StringResources.hpp>
#include <memory>

#include <util/Logger.hpp>

#include <cstdint>
#include <functional>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Notification Service //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Lifecycle, preferences and rendering belong to the host UI thread. Native
// activation callbacks only enqueue immutable arguments for that same owner.
class NotificationService : public std::enable_shared_from_this<NotificationService> {
public:
    using ReconnectRequestedCallback = std::function<void(winrt::hstring deviceId)>;
    using ShouldShowNotificationCallback = std::function<bool()>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit NotificationService(util::LogSink log,
                                 std::shared_ptr<StringResources const> strings,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
        : m_log(std::move(log)), m_strings(std::move(strings)), m_dispatcher(std::move(dispatcher)) {}
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

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void TeardownCore(bool clearCallbacks);
    struct Content;
    void ShowNotification(Content const& content, winrt::hstring const& id = {}, winrt::hstring const& deviceName = {});
    void OnNotificationInvoked(winrt::hstring const& argument);
    [[nodiscard]] bool ShouldShowNotifications() const;
    static winrt::fire_and_forget RemoveStaleStatusToastsAsync(
        winrt::Microsoft::Windows::AppNotifications::AppNotificationManager notificationManager,
        winrt::hstring group,
        winrt::hstring tagToRemove,
        util::LogSink log);
    bool ShowStatusToast(std::wstring const& xml, winrt::Windows::Foundation::DateTime const& expiration);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    winrt::Microsoft::Windows::AppNotifications::AppNotificationManager m_notificationManager{nullptr};
    winrt::event_token m_notificationInvokedToken{};
    ReconnectRequestedCallback m_reconnectCallback;
    ShouldShowNotificationCallback m_shouldShowNotificationCallback;
    winrt::hstring m_statusNotificationTag;
    bool m_showInProgress = false;
    bool m_notificationsRegistered = false;
    bool m_isTearingDown = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{nullptr};
    uint64_t m_registrationGeneration = 0;
};
