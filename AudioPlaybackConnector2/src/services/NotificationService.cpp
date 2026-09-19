#include <pch.h>
#include <services/NotificationService.hpp>
#include <core/StringResources.hpp>
#include <services/ToastContentBuilder.hpp>
#include <util/Util.hpp>

#include <utility>

namespace AppNotifications = winrt::Microsoft::Windows::AppNotifications;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

constexpr wchar_t kStatusNotificationGroup[] = L"audioPlaybackConnectorStatus";
constexpr wchar_t kStatusNotificationTagPrefix[] = L"currentStatus:";

std::wstring
NotificationText(StringResources const& strings, std::string_view key, std::wstring_view replacement = {}) {
    return util::ReplacePlaceholders(strings.Get(key), replacement);
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
    auto lifetime = shared_from_this();
    TeardownCore(false);

    try {
        if (!AppNotifications::AppNotificationManager::IsSupported()) {
            m_log.Trace(L"[NotificationService] AppNotificationManager is not supported; notifications disabled");
            return false;
        }

        auto notificationManager = AppNotifications::AppNotificationManager::Default();
        auto weak = weak_from_this();
        const auto generation = m_registrationGeneration;
        auto notificationInvokedToken = notificationManager.NotificationInvoked(
            [weak, dispatcher = m_dispatcher, generation, log = m_log](auto const&, auto const& args) noexcept {
                try {
                    const auto accepted = dispatcher.TryEnqueue([weak, generation, argument = args.Argument()] {
                        if (auto self = weak.lock();
                            self && !self->m_isTearingDown && generation == self->m_registrationGeneration) {
                            self->OnNotificationInvoked(argument);
                        }
                    });
                    if (!accepted) log.Trace(L"[NotificationService] activation discarded during dispatcher shutdown");
                } catch (...) {
                    log.UnknownException(L"[NotificationService] notification dispatch failed");
                }
            });
        bool registrationAttempted = false;
        auto registrationGuard = wil::scope_exit([&]() noexcept {
            if (notificationInvokedToken.value) {
                try {
                    notificationManager.NotificationInvoked(notificationInvokedToken);
                } catch (winrt::hresult_error const& ex) {
                    m_log.Exception(L"[NotificationService] Registration rollback callback revoke failed", ex);
                } catch (...) {
                    m_log.UnknownException(L"[NotificationService] Registration rollback callback revoke failed");
                }
            }
            if (registrationAttempted) {
                try {
                    notificationManager.Unregister();
                } catch (winrt::hresult_error const& ex) {
                    m_log.Exception(L"[NotificationService] Registration rollback unregister failed", ex);
                } catch (...) {
                    m_log.UnknownException(L"[NotificationService] Registration rollback unregister failed");
                }
            }
        });

        registrationAttempted = true;
        if (IsPackagedProcess()) {
            notificationManager.Register();
        } else {
            notificationManager.Register(appName, logoUri);
        }

        // Register may pump the UI queue; teardown must remain authoritative.
        if (generation != m_registrationGeneration) return false;

        m_notificationManager = notificationManager;
        m_notificationInvokedToken = notificationInvokedToken;
        m_notificationsRegistered = true;
        m_isTearingDown = false;
        registrationGuard.release();
        m_log.Trace(L"[NotificationService] AppNotificationManager registered");
        return true;
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[NotificationService] AppNotificationManager registration failed", ex);
        return false;
    } catch (std::exception const& ex) {
        m_log.Exception(L"[NotificationService] AppNotificationManager registration failed", ex);
        return false;
    } catch (...) {
        m_log.UnknownException(L"[NotificationService] AppNotificationManager registration failed");
        return false;
    }
}

void NotificationService::Teardown() noexcept {
    try {
        TeardownCore(true);
    } catch (...) {
    }
}

void NotificationService::TeardownCore(bool clearCallbacks) {
    m_isTearingDown = true;
    ++m_registrationGeneration;
    auto notificationManager = std::exchange(m_notificationManager, nullptr);
    auto notificationInvokedToken = std::exchange(m_notificationInvokedToken, {});
    const auto notificationsRegistered = std::exchange(m_notificationsRegistered, false);
    if (clearCallbacks) {
        m_reconnectCallback = nullptr;
        m_shouldShowNotificationCallback = nullptr;
    }
    m_statusNotificationTag = {};

    if (notificationManager && notificationInvokedToken.value) {
        try {
            notificationManager.NotificationInvoked(notificationInvokedToken);
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[NotificationService] Failed to revoke notification callback", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[NotificationService] Failed to revoke notification callback", ex);
        } catch (...) {
            m_log.UnknownException(L"[NotificationService] Failed to revoke notification callback");
        }
    }
    if (notificationManager && notificationsRegistered) {
        try {
            notificationManager.Unregister();
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[NotificationService] Failed to unregister notification manager", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[NotificationService] Failed to unregister notification manager", ex);
        } catch (...) {
            m_log.UnknownException(L"[NotificationService] Failed to unregister notification manager");
        }
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::SetReconnectCallback(ReconnectRequestedCallback callback) {
    m_reconnectCallback = std::move(callback);
}

void NotificationService::SetShouldShowNotificationCallback(ShouldShowNotificationCallback callback) {
    m_shouldShowNotificationCallback = std::move(callback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool NotificationService::ShouldShowNotifications() const {
    auto callback = m_shouldShowNotificationCallback;
    if (!callback) return true;
    try {
        return callback();
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[NotificationService] notification preference callback failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[NotificationService] notification preference callback failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[NotificationService] notification preference callback failed");
    }
    return false;
}

winrt::fire_and_forget
NotificationService::RemoveStaleStatusToastsAsync(AppNotifications::AppNotificationManager notificationManager,
                                                  winrt::hstring group,
                                                  winrt::hstring tagToRemove,
                                                  util::LogSink log) {
    try {
        if (!tagToRemove.empty()) co_await notificationManager.RemoveByTagAndGroupAsync(tagToRemove, group);
    } catch (winrt::hresult_error const& ex) {
        log.Exception(L"[NotificationService] stale notification removal failed", ex);
    } catch (std::exception const& ex) {
        log.Exception(L"[NotificationService] stale notification removal failed", ex);
    } catch (...) {
        log.UnknownException(L"[NotificationService] stale notification removal failed");
    }
}

bool NotificationService::ShowStatusToast(std::wstring const& xml,
                                          winrt::Windows::Foundation::DateTime const& expiration) {
    if (m_isTearingDown || !m_notificationManager || !m_notificationsRegistered || m_showInProgress) return false;
    auto lifetime = shared_from_this();
    m_showInProgress = true;
    auto finish = wil::scope_exit([&] { m_showInProgress = false; });
    const auto generation = m_registrationGeneration;
    auto manager = m_notificationManager;
    try {
        // Never reuse a tag across registrations: delayed removal may outlive
        // this service and must not delete a later service's notification.
        GUID identity{};
        winrt::check_hresult(CoCreateGuid(&identity));
        wchar_t identityText[39]{};
        StringFromGUID2(identity, identityText, static_cast<int>(std::size(identityText)));
        auto tag = winrt::hstring(kStatusNotificationTagPrefix) + identityText;
        AppNotifications::AppNotification notification{winrt::hstring(xml)};
        notification.Group(kStatusNotificationGroup);
        notification.Tag(tag);
        notification.Expiration(expiration);
        notification.ExpiresOnReboot(true);
        manager.Show(notification);
        if (m_isTearingDown || generation != m_registrationGeneration) {
            RemoveStaleStatusToastsAsync(manager, kStatusNotificationGroup, tag, m_log);
            return false;
        }
        auto previous = std::exchange(m_statusNotificationTag, std::move(tag));
        RemoveStaleStatusToastsAsync(manager, kStatusNotificationGroup, std::move(previous), m_log);
        return true;
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[NotificationService] AppNotificationManager.Show failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[NotificationService] AppNotificationManager.Show failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[NotificationService] AppNotificationManager.Show failed");
    }
    return false;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Show Notifications ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct NotificationService::Content {
    std::string_view title;
    std::string_view body;
    std::string_view caption;
    std::wstring_view image;
    std::wstring_view audio;
    std::string_view actionText;
    std::wstring_view action;
    std::wstring_view duration;
    std::chrono::seconds lifetime = std::chrono::minutes(1);
};

void NotificationService::ShowNotification(Content const& content,
                                           winrt::hstring const& id,
                                           winrt::hstring const& deviceName) {
    if (!ShouldShowNotifications()) return;
    auto xml = ToastXmlBuilder{};
    xml.Title(NotificationText(*m_strings, content.title, deviceName)).AppLogoOverride(content.image);
    if (!content.body.empty()) xml.Body(NotificationText(*m_strings, content.body));
    if (!content.caption.empty()) xml.Caption(NotificationText(*m_strings, content.caption));
    if (!content.action.empty()) {
        xml.Action(NotificationText(*m_strings, content.actionText),
                   ToastArguments{}.Action(content.action).DeviceId(id));
    }
    if (content.audio.empty())
        xml.SilentAudio();
    else
        xml.Audio(content.audio);
    xml.Duration(content.duration);
    ShowStatusToast(xml.Build(), ExpirationFromNow(content.lifetime));
}

void NotificationService::ShowAppStarted() {
    ShowNotification({.title = "Notification_AppStarted_Title",
                      .body = "Notification_AppStarted_Body",
                      .image = L"ms-appx:///Images/ToastInfo.png",
                      .lifetime = std::chrono::seconds(7)});
}

void NotificationService::ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName) {
    ShowNotification({.title = "Notification_Connected",
                      .caption = "Notification_Connected_Caption",
                      .image = L"ms-appx:///Images/ToastConnected.png",
                      .audio = L"ms-winsoundevent:Notification.Default",
                      .actionText = "Reconnect",
                      .action = L"reconnect",
                      .duration = L"long"},
                     id,
                     deviceName);
}

void NotificationService::ShowDeviceDisconnected(winrt::hstring const& id, winrt::hstring const& deviceName) {
    ShowNotification({.title = "Notification_Disconnected",
                      .body = "Notification_Disconnected_Body",
                      .image = L"ms-appx:///Images/ToastWarning.png"},
                     id,
                     deviceName);
}

void NotificationService::ShowAutoReconnect(winrt::hstring const& id, winrt::hstring const& deviceName) {
    ShowNotification({.title = "Notification_AutoReconnect",
                      .body = "Notification_AutoReconnect_Body",
                      .image = L"ms-appx:///Images/ToastReconnect.png"},
                     id,
                     deviceName);
}

void NotificationService::ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName) {
    ShowNotification({.title = "Notification_AutoReconnectFailed_Title",
                      .body = "Notification_AutoReconnectFailed_Body",
                      .image = L"ms-appx:///Images/ToastError.png",
                      .audio = L"ms-winsoundevent:Notification.Looping.Alarm2",
                      .actionText = "Notification_Retry",
                      .action = L"retry",
                      .lifetime = std::chrono::hours(1)},
                     id,
                     deviceName);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handler /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::OnNotificationInvoked(winrt::hstring const& argument) {
    try {
        auto parsedArguments = ToastArguments::Parse(argument);
        auto action = ToastArguments::Find(parsedArguments, L"action");
        auto deviceId = ToastArguments::Find(parsedArguments, L"deviceId");

        if (!deviceId) {
            m_log.Trace(L"[NotificationService] App notification invoked without deviceId: {0}",
                        std::wstring(argument));
            return;
        }

        m_log.Trace(L"[NotificationService] App notification invoked: action={0}, deviceId={1}",
                    action.value_or(L""),
                    *deviceId);

        if (action && (*action == L"reconnect" || *action == L"retry")) {
            if (m_isTearingDown) return;
            auto reconnectCallback = m_reconnectCallback;
            if (reconnectCallback) reconnectCallback(winrt::hstring(*deviceId));
        }
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[NotificationService] App notification activation failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[NotificationService] App notification activation failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[NotificationService] App notification activation failed");
    }
}
