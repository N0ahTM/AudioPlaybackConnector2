#include <pch.h>
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <winrt/Windows.Foundation.h>
#include <services/NotificationService.hpp>
#include <app/AppController.hpp>
#include <app/RatingPromptChannel.hpp>
#include <app/RatingPromptPolicy.hpp>
#include <algorithm>
#include <type_traits>
#include <variant>
#include <core/StringResources.hpp>
#include <services/ToastContentBuilder.hpp>
#include <cstddef>
#include <string>
#include <string_view>

#include <utility>
#include <winrt/Windows.System.h>

namespace AppNotifications = winrt::Microsoft::Windows::AppNotifications;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

constexpr wchar_t kStatusNotificationGroup[] = L"audioPlaybackConnectorStatus";
constexpr wchar_t kStatusNotificationTagPrefix[] = L"currentStatus:";

std::wstring
NotificationText(StringResources const& strings, std::string_view key, std::wstring_view replacement = {}) {
    const auto text = strings.Get(key);
    std::wstring result;
    std::size_t position = 0;
    for (;;) {
        const auto placeholder = text.find(L"{0}", position);
        if (placeholder == std::wstring::npos) {
            result.append(text, position);
            return result;
        }
        result.append(text, position, placeholder - position);
        result.append(replacement);
        position = placeholder + 3;
    }
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
    Teardown();

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

void NotificationService::Teardown() noexcept try {
    m_isTearingDown = true;
    ++m_registrationGeneration;
    auto notificationManager = std::exchange(m_notificationManager, nullptr);
    auto notificationInvokedToken = std::exchange(m_notificationInvokedToken, {});
    const auto notificationsRegistered = std::exchange(m_notificationsRegistered, false);
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
} catch (...) {
    m_log.UnknownException(L"[NotificationService] Teardown failed");
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
    auto controller = m_controller.lock();
    if (m_isTearingDown || !controller) return;
    auto const snapshot = controller->Snapshot();
    if (!snapshot.IsRunning || !snapshot.Settings.ShowNotifications) return;
    ShowNotification({.title = "Notification_AppStarted_Title",
                      .body = "Notification_AppStarted_Body",
                      .image = L"ms-appx:///Images/ToastInfo.png",
                      .lifetime = std::chrono::seconds(7)});
}

void NotificationService::MaybeShowRatingPrompt() noexcept {
    try {
        auto controller = m_controller.lock();
        if (m_isTearingDown || !controller) return;
        auto const snapshot = controller->Snapshot();
        if (!snapshot.IsRunning || !snapshot.Settings.ShowNotifications) return;
        if (!apc::app::IsRatingPromptEligible(
                apc::app::IsStoreChannel(), snapshot.Settings.RatingPrompt, apc::app::TodayLocalIsoDate()))
            return;
        auto xml = ToastXmlBuilder{};
        xml.Title(NotificationText(*m_strings, "RatingPrompt_Title"))
            .AppLogoOverride(L"ms-appx:///Images/ToastInfo.png")
            .Body(NotificationText(*m_strings, "RatingPrompt_Body"))
            .Action(NotificationText(*m_strings, "RatingPrompt_Rate"), ToastArguments{}.Action(L"rate"))
            .SilentAudio()
            .Duration(L"short");
        if (!ShowStatusToast(xml.Build(), ExpirationFromNow(std::chrono::minutes(10)))) return;
        // Mark as asked only once the notification was actually presented.
        (void)controller->MarkRatingPromptShown();
    } catch (...) {
        // The rating prompt is best effort and must never break presentation.
    }
}

void NotificationService::HandleEvent(apc::app::AppEvent const& notification) noexcept try {
    auto controller = m_controller.lock();
    if (m_isTearingDown || !controller) return;
    std::visit(
        [&](auto const& event) {
            using namespace apc::app;
            using T = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<T, DeviceConnectedEvent> || std::is_same_v<T, DeviceDisconnectedEvent> ||
                          std::is_same_v<T, AutoReconnectTriggeredEvent> ||
                          std::is_same_v<T, AutoReconnectFailedEvent>) {
                if constexpr (std::is_same_v<T, DeviceDisconnectedEvent>) {
                    if (!event.NotifyUser) return;
                }
                auto const snapshot = controller->Snapshot();
                if (!snapshot.IsRunning || !snapshot.Settings.ShowNotifications) return;
                if constexpr (std::is_same_v<T, DeviceConnectedEvent>) {
                    if (!std::ranges::any_of(snapshot.Devices, [&](auto const& device) {
                            return device.Id.View() == event.Id.View() && device.IsConnected;
                        }))
                        return;
                }
                auto const id = winrt::hstring(event.Id.View());
                auto const& settings = snapshot.Settings;
                auto const device = std::ranges::find_if(
                    settings.Devices, [&](auto const& value) { return value.Id == event.Id.View(); });
                // Alias, privacy and name come from one revision, including removed devices.
                auto const name = [&]() -> winrt::hstring {
                    if (device != settings.Devices.end() && !device->Alias.empty())
                        return winrt::hstring(device->Alias);
                    if (snapshot.PrivacyModeEnabled) return winrt::hstring(m_strings->Get("Privacy_RedactedDevice"));
                    if (device != settings.Devices.end() && !device->Name.empty()) return winrt::hstring(device->Name);
                    return id;
                }();
                Content content;
                if constexpr (std::is_same_v<T, DeviceConnectedEvent>) {
                    content = {.title = "Notification_Connected",
                               .caption = "Notification_Connected_Caption",
                               .image = L"ms-appx:///Images/ToastConnected.png",
                               .audio = L"ms-winsoundevent:Notification.Default",
                               .actionText = "Reconnect",
                               .action = L"reconnect",
                               .duration = L"long"};
                } else if constexpr (std::is_same_v<T, DeviceDisconnectedEvent>) {
                    content = {.title = "Notification_Disconnected",
                               .body = "Notification_Disconnected_Body",
                               .image = L"ms-appx:///Images/ToastWarning.png"};
                } else if constexpr (std::is_same_v<T, AutoReconnectTriggeredEvent>) {
                    content = {.title = "Notification_AutoReconnect",
                               .body = "Notification_AutoReconnect_Body",
                               .image = L"ms-appx:///Images/ToastReconnect.png"};
                } else {
                    content = {.title = "Notification_AutoReconnectFailed_Title",
                               .body = "Notification_AutoReconnectFailed_Body",
                               .image = L"ms-appx:///Images/ToastError.png",
                               .audio = L"ms-winsoundevent:Notification.Looping.Alarm2",
                               .actionText = "Notification_Retry",
                               .action = L"retry",
                               .lifetime = std::chrono::hours(1)};
                }
                ShowNotification(content, id, name);
            }
        },
        notification);
} catch (winrt::hresult_error const& ex) {
    m_log.Exception(L"[NotificationService] Device notification failed", ex);
} catch (std::exception const& ex) {
    m_log.Exception(L"[NotificationService] Device notification failed", ex);
} catch (...) {
    m_log.UnknownException(L"[NotificationService] Device notification failed");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handler /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::fire_and_forget NotificationService::OpenStoreReviewPage() {
    try {
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(L"ms-windows-store://review/?ProductId=9N366PGKJZ0K"));
    } catch (...) {
        // Opening the review page is best effort; a failed launch needs no surface.
    }
}

void NotificationService::OnNotificationInvoked(winrt::hstring const& argument) {
    try {
        auto parsedArguments = ToastArguments::Parse(argument);
        auto action = ToastArguments::Find(parsedArguments, L"action");
        auto deviceId = ToastArguments::Find(parsedArguments, L"deviceId");

        if (action && *action == L"rate") {
            OpenStoreReviewPage();
            return;
        }

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
            auto controller = m_controller.lock();
            auto selector = apc::app::DeviceSelector::ById(*deviceId);
            if (controller && selector) {
                (void)controller->Reconnect(std::move(*selector), apc::app::AppCommandContext::Detached());
            }
        }
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[NotificationService] App notification activation failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[NotificationService] App notification activation failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[NotificationService] App notification activation failed");
    }
}
