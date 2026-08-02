#include <pch.h>
#include <services/NotificationService.hpp>
#include <core/StringResources.hpp>
#include <services/ToastContentBuilder.hpp>
#include <services/UpdateService.hpp>
#include <util/Util.hpp>

#include <utility>

namespace AppNotifications = winrt::Microsoft::Windows::AppNotifications;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

constexpr wchar_t kStatusNotificationGroup[] = L"audioPlaybackConnectorStatus";
constexpr wchar_t kStatusNotificationTagPrefix[] = L"currentStatus:";

std::wstring NotificationText(std::string_view key, std::wstring_view replacement = {}) {
    return util::ReplacePlaceholders(std::wstring(_(key)), replacement);
}

winrt::Windows::Foundation::DateTime ExpirationFromNow(std::chrono::seconds seconds) {
    return winrt::clock::now() + seconds;
}

bool IsPackagedProcess() {
    UINT32 length = 0;
    const auto result = GetCurrentPackageFullName(&length, nullptr);
    return result != APPMODEL_ERROR_NO_PACKAGE;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

NotificationService::~NotificationService() {
    Teardown();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool NotificationService::Initialize(winrt::hstring const& appName, winrt::Windows::Foundation::Uri const& logoUri) {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    TeardownCore(false);

    try {
        if (!AppNotifications::AppNotificationManager::IsSupported()) {
            DebugTrace(L"[NotificationService] AppNotificationManager is not supported; notifications disabled");
            return false;
        }

        auto notificationManager = AppNotifications::AppNotificationManager::Default();
        auto weak = weak_from_this();
        auto notificationInvokedToken = notificationManager.NotificationInvoked([weak](auto const&, auto const& args) {
            if (auto self = weak.lock()) {
                self->OnNotificationInvoked(args);
            }
        });
        bool registrationAttempted = false;
        auto registrationGuard = wil::scope_exit([&]() noexcept {
            try {
                if (notificationInvokedToken.value) {
                    notificationManager.NotificationInvoked(notificationInvokedToken);
                }
                if (registrationAttempted) {
                    notificationManager.Unregister();
                }
            } catch (...) {
            }
        });

        registrationAttempted = true;
        if (IsPackagedProcess()) {
            notificationManager.Register();
        } else {
            notificationManager.Register(appName, logoUri);
        }

        {
            auto guard = m_lock.lock_exclusive();
            m_notificationManager = notificationManager;
            m_notificationInvokedToken = notificationInvokedToken;
            m_notificationsRegistered = true;
            m_isTearingDown = false;
        }
        registrationGuard.release();
        DebugTrace(L"[NotificationService] AppNotificationManager registered");
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager registration failed", ex);
        return false;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager registration failed", ex);
        return false;
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] AppNotificationManager registration failed");
        return false;
    }
}

void NotificationService::Teardown() noexcept {
    try {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        TeardownCore(true);
    } catch (...) {
    }
}

void NotificationService::TeardownCore(bool clearCallbacks) {
    std::lock_guard statusLock(m_statusNotificationMutex);
    AppNotifications::AppNotificationManager notificationManager{nullptr};
    winrt::event_token notificationInvokedToken{};
    bool notificationsRegistered = false;

    {
        auto guard = m_lock.lock_exclusive();
        m_isTearingDown = true;
        notificationManager = std::exchange(m_notificationManager, nullptr);
        notificationInvokedToken = std::exchange(m_notificationInvokedToken, {});
        notificationsRegistered = std::exchange(m_notificationsRegistered, false);
        if (clearCallbacks) {
            m_reconnectCallback = nullptr;
            m_shouldShowNotificationCallback = nullptr;
        }
        m_statusNotificationTags.clear();
        ++m_statusNotificationGeneration;
    }

    try {
        if (notificationManager && notificationInvokedToken.value) {
            notificationManager.NotificationInvoked(notificationInvokedToken);
        }
        if (notificationManager && notificationsRegistered) {
            notificationManager.Unregister();
        }
    } catch (...) {
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::SetReconnectCallback(ReconnectRequestedCallback callback) {
    auto guard = m_lock.lock_exclusive();
    m_reconnectCallback = std::move(callback);
}

void NotificationService::SetShouldShowNotificationCallback(ShouldShowNotificationCallback callback) {
    auto guard = m_lock.lock_exclusive();
    m_shouldShowNotificationCallback = std::move(callback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

NotificationService::StatusNotificationTagReservation NotificationService::ReserveStatusNotificationTag() {
    auto guard = m_lock.lock_exclusive();
    const auto generation =
        m_statusNotificationGeneration == std::numeric_limits<uint64_t>::max() ? 1 : m_statusNotificationGeneration + 1;
    auto currentTag = winrt::hstring(kStatusNotificationTagPrefix) + winrt::hstring(std::to_wstring(generation));
    std::vector<winrt::hstring> nextTags{currentTag};

    StatusNotificationTagReservation reservation;
    reservation.TagsToRemove = std::move(m_statusNotificationTags);
    reservation.CurrentTag = std::move(currentTag);
    reservation.Generation = generation;

    m_statusNotificationTags = std::move(nextTags);
    m_statusNotificationGeneration = generation;
    return reservation;
}

void NotificationService::RollbackStatusNotificationTag(StatusNotificationTagReservation&& reservation) {
    auto guard = m_lock.lock_exclusive();
    if (reservation.Generation == m_statusNotificationGeneration) {
        m_statusNotificationTags = std::move(reservation.TagsToRemove);
    }
}

bool NotificationService::ShouldShowNotifications() const {
    ShouldShowNotificationCallback callback;
    {
        auto guard = m_lock.lock_shared();
        callback = m_shouldShowNotificationCallback;
    }
    if (!callback) return true;
    try {
        return callback();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] notification preference callback failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] notification preference callback failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] notification preference callback failed");
    }
    return false;
}

winrt::fire_and_forget
NotificationService::RemoveStaleStatusToastsAsync(AppNotifications::AppNotificationManager notificationManager,
                                                  winrt::hstring group,
                                                  // cppcheck-suppress passedByValue
                                                  std::vector<winrt::hstring> tagsToRemove) {
    try {
        auto lifetime = shared_from_this();
        for (auto const& tagToRemove : tagsToRemove) {
            if (!tagToRemove.empty()) co_await notificationManager.RemoveByTagAndGroupAsync(tagToRemove, group);
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] stale notification removal failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] stale notification removal failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] stale notification removal failed");
    }
}

bool NotificationService::ShowStatusToast(std::wstring const& xml,
                                          winrt::Windows::Foundation::DateTime const& expiration) {
    AppNotifications::AppNotificationManager notificationManager{nullptr};
    StatusNotificationTagReservation reservation;
    {
        std::lock_guard statusLock(m_statusNotificationMutex);
        {
            auto guard = m_lock.lock_shared();
            if (m_isTearingDown || !m_notificationManager || !m_notificationsRegistered) {
                return false;
            }
            notificationManager = m_notificationManager;
        }

        try {
            reservation = ReserveStatusNotificationTag();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[NotificationService] failed to reserve notification tag", ex);
            return false;
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[NotificationService] failed to reserve notification tag", ex);
            return false;
        } catch (...) {
            util::DebugTraceUnknownException(L"[NotificationService] failed to reserve notification tag");
            return false;
        }
        try {
            AppNotifications::AppNotification notification{winrt::hstring(xml)};
            notification.Group(kStatusNotificationGroup);
            notification.Tag(reservation.CurrentTag);
            notification.Expiration(expiration);
            notification.ExpiresOnReboot(true);
            notificationManager.Show(notification);
        } catch (winrt::hresult_error const& ex) {
            RollbackStatusNotificationTag(std::move(reservation));
            util::DebugTraceException(L"[NotificationService] AppNotificationManager.Show failed", ex);
            return false;
        } catch (std::exception const& ex) {
            RollbackStatusNotificationTag(std::move(reservation));
            util::DebugTraceException(L"[NotificationService] AppNotificationManager.Show failed", ex);
            return false;
        } catch (...) {
            RollbackStatusNotificationTag(std::move(reservation));
            util::DebugTraceUnknownException(L"[NotificationService] AppNotificationManager.Show failed");
            return false;
        }
    }

    try {
        RemoveStaleStatusToastsAsync(
            notificationManager, kStatusNotificationGroup, std::move(reservation.TagsToRemove));
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] failed to schedule stale notification removal");
    }
    return true;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Show Notifications ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::ShowAppStarted() {
    if (!ShouldShowNotifications()) return;
    auto title = NotificationText("Notification_AppStarted_Title");
    auto body = NotificationText("Notification_AppStarted_Body");
    auto xml = ToastXmlBuilder{}
                   .Title(title)
                   .Body(body)
                   .AppLogoOverride(L"ms-appx:///Images/ToastInfo.png")
                   .SilentAudio()
                   .Build();
    ShowStatusToast(xml, ExpirationFromNow(std::chrono::seconds(7)));
}

void NotificationService::ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName) {
    if (!ShouldShowNotifications()) return;
    auto title = NotificationText("Notification_Connected", deviceName);
    auto xml = ToastXmlBuilder{}
                   .Title(title)
                   .Caption(NotificationText("Notification_Connected_Caption"))
                   .Action(NotificationText("Reconnect"), ToastArguments{}.Action(L"reconnect").DeviceId(id))
                   .AppLogoOverride(L"ms-appx:///Images/ToastConnected.png")
                   .Audio(L"ms-winsoundevent:Notification.Default")
                   .Duration(L"long")
                   .Build();

    ShowStatusToast(xml, ExpirationFromNow(std::chrono::minutes(1)));
}

void NotificationService::ShowDeviceDisconnected(winrt::hstring const&, winrt::hstring const& deviceName) {
    if (!ShouldShowNotifications()) return;
    auto title = NotificationText("Notification_Disconnected", deviceName);
    auto xml = ToastXmlBuilder{}
                   .Title(title)
                   .Body(NotificationText("Notification_Disconnected_Body"))
                   .AppLogoOverride(L"ms-appx:///Images/ToastWarning.png")
                   .SilentAudio()
                   .Build();

    ShowStatusToast(xml, ExpirationFromNow(std::chrono::minutes(1)));
}

void NotificationService::ShowAutoReconnect(winrt::hstring const&, winrt::hstring const& deviceName) {
    if (!ShouldShowNotifications()) return;
    auto title = NotificationText("Notification_AutoReconnect", deviceName);
    auto xml = ToastXmlBuilder{}
                   .Title(title)
                   .Body(NotificationText("Notification_AutoReconnect_Body"))
                   .AppLogoOverride(L"ms-appx:///Images/ToastReconnect.png")
                   .SilentAudio()
                   .Build();

    ShowStatusToast(xml, ExpirationFromNow(std::chrono::minutes(1)));
}

void NotificationService::ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName) {
    if (!ShouldShowNotifications()) return;
    auto title = NotificationText("Notification_AutoReconnectFailed_Title", deviceName);
    auto xml = ToastXmlBuilder{}
                   .Title(title)
                   .Body(NotificationText("Notification_AutoReconnectFailed_Body"))
                   .Action(NotificationText("Notification_Retry"), ToastArguments{}.Action(L"retry").DeviceId(id))
                   .AppLogoOverride(L"ms-appx:///Images/ToastError.png")
                   .Audio(L"ms-winsoundevent:Notification.Looping.Alarm2")
                   .Build();

    ShowStatusToast(xml, ExpirationFromNow(std::chrono::hours(1)));
}

bool NotificationService::ShowUpdateAvailable(std::wstring const& latestVersion) {
    if (!ShouldShowNotifications()) return false;
    auto title = NotificationText("Notification_UpdateAvailable_Title", latestVersion);
    auto body = NotificationText("Notification_UpdateAvailable_Body");
    auto xml =
        ToastXmlBuilder{}
            .Title(title)
            .Body(body)
            .Action(NotificationText("Notification_UpdateAvailable_Action"), ToastArguments{}.Action(L"openUpdate"))
            .AppLogoOverride(L"ms-appx:///Images/ToastInfo.png")
            .SilentAudio()
            .Build();

    return ShowStatusToast(xml, ExpirationFromNow(std::chrono::hours(6)));
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handler /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::OnNotificationInvoked(AppNotifications::AppNotificationActivatedEventArgs const& args) {
    try {
        auto parsedArguments = ToastArguments::Parse(args.Argument());
        auto action = ToastArguments::Find(parsedArguments, L"action");
        auto deviceId = ToastArguments::Find(parsedArguments, L"deviceId");

        if (action && *action == L"openUpdate") {
            auto guard = m_lock.lock_shared();
            if (m_isTearingDown) return;
            DebugTrace(L"[NotificationService] App notification invoked: action=openUpdate");
            UpdateService::LaunchAppInstallerAsync();
            return;
        }

        if (!deviceId) {
            DebugTrace(L"[NotificationService] App notification invoked without deviceId: {0}",
                       std::wstring(args.Argument()));
            return;
        }

        DebugTrace(L"[NotificationService] App notification invoked: action={0}, deviceId={1}",
                   action.value_or(L""),
                   *deviceId);

        if (action && (*action == L"reconnect" || *action == L"retry")) {
            auto guard = m_lock.lock_shared();
            if (m_isTearingDown) return;
            if (m_reconnectCallback) m_reconnectCallback(winrt::hstring(*deviceId));
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] App notification activation failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] App notification activation failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] App notification activation failed");
    }
}
