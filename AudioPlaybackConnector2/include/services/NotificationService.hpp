#pragma once

#include <core/StringResources.hpp>
#include <app/AppModels.hpp>
#include <memory>

#include <util/Logger.hpp>

#include <cstdint>
#include <utility>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

namespace apc::app {
class AppController;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Notification Service //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Lifecycle, preferences and rendering belong to the host UI thread. Native
// activation callbacks only enqueue immutable arguments for that same owner.
class NotificationService : public std::enable_shared_from_this<NotificationService> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit NotificationService(util::LogSink log,
                                 std::shared_ptr<StringResources const> strings,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                 std::weak_ptr<apc::app::AppController> controller)
        : m_log(std::move(log)), m_strings(std::move(strings)), m_dispatcher(std::move(dispatcher)),
          m_controller(std::move(controller)) {}
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    [[nodiscard]] bool Initialize(winrt::hstring const& appName, winrt::Windows::Foundation::Uri const& logoUri);
    void Teardown() noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Notifications /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowAppStarted();
    void HandleEvent(apc::app::AppEvent const& event) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct Content;
    void ShowNotification(Content const& content, winrt::hstring const& id = {}, winrt::hstring const& deviceName = {});
    void OnNotificationInvoked(winrt::hstring const& argument);
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
    winrt::hstring m_statusNotificationTag;
    bool m_showInProgress = false;
    bool m_notificationsRegistered = false;
    bool m_isTearingDown = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{nullptr};
    uint64_t m_registrationGeneration = 0;
    // The host owns both components; toast callbacks must not retain the controller.
    std::weak_ptr<apc::app::AppController> m_controller;
};
