#include <pch.h>
#include <services/NotificationService.hpp>
#include <core/StringResources.hpp>
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

std::wstring ReplacePlaceholders(std::wstring_view templateStr, std::wstring_view replacement) {
    std::wstring result;
    size_t pos = 0;
    while (pos < templateStr.size()) {
        auto found = templateStr.find(L"{0}", pos);
        if (found == std::wstring_view::npos) {
            result.append(templateStr.substr(pos));
            break;
        }
        result.append(templateStr.substr(pos, found - pos));
        result.append(replacement);
        pos = found + 3;
    }
    return result;
}

std::wstring NotificationText(std::string_view key, std::wstring_view replacement = {}) {
    return ReplacePlaceholders(std::wstring(_(key)), replacement);
}

std::wstring XmlEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (auto ch : value) {
        switch (ch) {
            case L'&': result += L"&amp;"; break;
            case L'<': result += L"&lt;"; break;
            case L'>': result += L"&gt;"; break;
            case L'"': result += L"&quot;"; break;
            case L'\'': result += L"&apos;"; break;
            default: result += ch; break;
        }
    }
    return result;
}

winrt::hstring ToastArguments(std::wstring_view action, winrt::hstring const& deviceId = {}) {
    auto arguments =
        winrt::hstring(L"action=") + winrt::Windows::Foundation::Uri::EscapeComponent(winrt::hstring(action));
    if (!deviceId.empty()) {
        arguments = arguments + L"&deviceId=" + winrt::Windows::Foundation::Uri::EscapeComponent(deviceId);
    }
    return arguments;
}

std::wstring UrlDecodeComponent(std::wstring_view value) {
    std::wstring normalized(value);
    std::replace(normalized.begin(), normalized.end(), L'+', L' ');

    try {
        return std::wstring(winrt::Windows::Foundation::Uri::UnescapeComponent(winrt::hstring(normalized)));
    } catch (...) {
        return normalized;
    }
}

std::unordered_map<std::wstring, std::wstring> ParseToastArgumentString(winrt::hstring const& rawArguments) {
    std::unordered_map<std::wstring, std::wstring> result;
    std::wstring_view raw(rawArguments.c_str(), rawArguments.size());

    size_t start = 0;
    while (start <= raw.size()) {
        const auto separator = raw.find(L'&', start);
        const auto end = separator == std::wstring_view::npos ? raw.size() : separator;
        const auto pair = raw.substr(start, end - start);

        if (!pair.empty()) {
            const auto equals = pair.find(L'=');
            const auto key = equals == std::wstring_view::npos ? pair : pair.substr(0, equals);
            const auto value = equals == std::wstring_view::npos ? std::wstring_view{} : pair.substr(equals + 1);
            auto decodedKey = UrlDecodeComponent(key);
            if (!decodedKey.empty()) {
                result[std::move(decodedKey)] = UrlDecodeComponent(value);
            }
        }

        if (separator == std::wstring_view::npos) break;
        start = separator + 1;
    }

    return result;
}

std::optional<std::wstring> FindToastArgument(std::unordered_map<std::wstring, std::wstring> const& arguments,
                                              std::wstring_view key) {
    auto it = arguments.find(std::wstring(key));
    if (it == arguments.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

std::wstring_view AsView(winrt::hstring const& value) {
    return std::wstring_view(value.c_str(), value.size());
}

winrt::Windows::Foundation::DateTime ExpirationFromNow(std::chrono::seconds seconds) {
    return winrt::clock::now() + seconds;
}

bool IsPackagedProcess() {
    UINT32 length = 0;
    const auto result = GetCurrentPackageFullName(&length, nullptr);
    return result != APPMODEL_ERROR_NO_PACKAGE;
}

std::wstring BuildToastXml(std::wstring_view title,
                           std::wstring_view body,
                           std::wstring_view caption,
                           std::wstring_view actionText,
                           winrt::hstring const& actionArgs,
                           std::wstring_view appLogoOverride,
                           std::wstring_view audioXml,
                           std::wstring_view duration = {}) {
    std::wstring xml = L"<toast";
    if (!duration.empty()) {
        xml += L" duration=\"";
        xml += XmlEscape(duration);
        xml += L"\"";
    }
    xml += L"><visual><binding template=\"ToastGeneric\">";
    if (!appLogoOverride.empty()) {
        xml += L"<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"";
        xml += XmlEscape(appLogoOverride);
        xml += L"\"/>";
    }
    xml += L"<text>";
    xml += XmlEscape(title);
    xml += L"</text>";
    if (!body.empty()) {
        xml += L"<text>";
        xml += XmlEscape(body);
        xml += L"</text>";
    }
    if (!caption.empty()) {
        xml += L"<text hint-style=\"caption\" hint-color=\"secondary\">";
        xml += XmlEscape(caption);
        xml += L"</text>";
    }
    xml += L"</binding></visual>";
    if (!actionText.empty()) {
        xml += L"<actions><action content=\"";
        xml += XmlEscape(actionText);
        xml += L"\" arguments=\"";
        xml += XmlEscape(AsView(actionArgs));
        xml += L"\"/></actions>";
    }
    xml += audioXml;
    xml += L"</toast>";
    return xml;
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
    try {
        if (!AppNotifications::AppNotificationManager::IsSupported()) {
            DebugTrace(L"[NotificationService] AppNotificationManager is not supported; using tray balloon fallback");
            return false;
        }

        {
            auto guard = m_lock.lock_exclusive();
            m_isTearingDown = false;
        }

        auto notificationManager = AppNotifications::AppNotificationManager::Default();
        auto weak = weak_from_this();
        auto notificationInvokedToken = notificationManager.NotificationInvoked([weak](auto const&, auto const& args) {
            if (auto self = weak.lock()) {
                self->OnNotificationInvoked(args);
            }
        });

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
        }
        DebugTrace(L"[NotificationService] AppNotificationManager registered");
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager registration failed", ex);
        Teardown();
        return false;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager registration failed", ex);
        Teardown();
        return false;
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] AppNotificationManager registration failed");
        Teardown();
        return false;
    }
}

void NotificationService::Teardown() noexcept {
    AppNotifications::AppNotificationManager notificationManager{nullptr};
    winrt::event_token notificationInvokedToken{};
    bool notificationsRegistered = false;

    {
        auto guard = m_lock.lock_exclusive();
        m_isTearingDown = true;
        notificationManager = std::exchange(m_notificationManager, nullptr);
        notificationInvokedToken = std::exchange(m_notificationInvokedToken, {});
        notificationsRegistered = std::exchange(m_notificationsRegistered, false);
        m_reconnectCallback = nullptr;
        m_fallbackNotifier = nullptr;
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

void NotificationService::SetFallbackNotifier(FallbackNotificationCallback callback) {
    auto guard = m_lock.lock_exclusive();
    m_fallbackNotifier = std::move(callback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

NotificationService::StatusNotificationTagReservation NotificationService::ReserveStatusNotificationTag() {
    auto guard = m_lock.lock_exclusive();
    const auto generation = ++m_statusNotificationGeneration;

    StatusNotificationTagReservation reservation;
    reservation.TagsToRemove = std::move(m_statusNotificationTags);
    reservation.CurrentTag = winrt::hstring(kStatusNotificationTagPrefix) + winrt::hstring(std::to_wstring(generation));
    reservation.Generation = generation;

    m_statusNotificationTags.clear();
    m_statusNotificationTags.push_back(reservation.CurrentTag);
    return reservation;
}

bool NotificationService::IsStatusNotificationGenerationCurrent(uint64_t generation) const {
    auto guard = m_lock.lock_shared();
    return !m_isTearingDown && generation == m_statusNotificationGeneration;
}

winrt::fire_and_forget NotificationService::ShowToastOrFallbackAsync(std::wstring xml,
                                                                     winrt::hstring group,
                                                                     winrt::hstring tag,
                                                                     // cppcheck-suppress passedByValue
                                                                     std::vector<winrt::hstring> tagsToRemove,
                                                                     uint64_t generation,
                                                                     winrt::Windows::Foundation::DateTime expiration,
                                                                     // cppcheck-suppress passedByValue
                                                                     std::wstring fallbackTitle,
                                                                     // cppcheck-suppress passedByValue
                                                                     std::wstring fallbackBody,
                                                                     FallbackNotificationType fallbackType) {
    auto lifetime = shared_from_this();
    AppNotifications::AppNotificationManager notificationManager{nullptr};
    bool useFallback = false;
    {
        auto guard = m_lock.lock_shared();
        if (m_isTearingDown || !m_notificationManager || !m_notificationsRegistered) {
            useFallback = true;
        } else {
            notificationManager = m_notificationManager;
        }
    }

    if (useFallback) {
        ShowFallbackNotification(fallbackTitle, fallbackBody, fallbackType);
        co_return;
    }

    try {
        for (auto const& tagToRemove : tagsToRemove) {
            if (!tagToRemove.empty() && tagToRemove != tag) {
                co_await notificationManager.RemoveByTagAndGroupAsync(tagToRemove, group);
            }
        }

        if (!IsStatusNotificationGenerationCurrent(generation)) co_return;

        AppNotifications::AppNotification notification{winrt::hstring(xml)};
        notification.Group(group);
        notification.Tag(tag);
        notification.Expiration(expiration);
        notification.ExpiresOnReboot(true);
        notificationManager.Show(notification);
        co_return;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager.Show failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] AppNotificationManager.Show failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] AppNotificationManager.Show failed");
    }

    if (IsStatusNotificationGenerationCurrent(generation)) {
        ShowFallbackNotification(fallbackTitle, fallbackBody, fallbackType);
    }
}

void NotificationService::ShowStatusToastOrFallback(std::wstring const& xml,
                                                    winrt::Windows::Foundation::DateTime const& expiration,
                                                    std::wstring const& fallbackTitle,
                                                    std::wstring const& fallbackBody,
                                                    FallbackNotificationType fallbackType) {
    auto reservation = ReserveStatusNotificationTag();
    ShowToastOrFallbackAsync(std::wstring(xml),
                             kStatusNotificationGroup,
                             reservation.CurrentTag,
                             std::move(reservation.TagsToRemove),
                             reservation.Generation,
                             expiration,
                             fallbackTitle,
                             fallbackBody,
                             fallbackType);
}

void NotificationService::ShowFallbackNotification(std::wstring const& fallbackTitle,
                                                   std::wstring const& fallbackBody,
                                                   FallbackNotificationType fallbackType) {
    FallbackNotificationCallback fallbackNotifier;
    {
        auto guard = m_lock.lock_shared();
        if (m_isTearingDown) return;
        fallbackNotifier = m_fallbackNotifier;
    }

    if (fallbackNotifier) {
        fallbackNotifier(fallbackTitle, fallbackBody, fallbackType);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Show Notifications ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::ShowAppStarted() {
    auto title = NotificationText("Notification_AppStarted_Title");
    auto body = NotificationText("Notification_AppStarted_Body");
    auto xml =
        BuildToastXml(title, body, L"", L"", L"", L"ms-appx:///Images/ToastInfo.png", L"<audio silent=\"true\"/>");
    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::seconds(7)),
                              std::wstring(_("AppName")),
                              body,
                              FallbackNotificationType::Info);
}

void NotificationService::ShowDeviceConnected(winrt::hstring const& id, winrt::hstring const& deviceName) {
    auto title = NotificationText("Notification_Connected", deviceName);
    auto xml = BuildToastXml(title,
                             L"",
                             NotificationText("Notification_Connected_Caption"),
                             NotificationText("Reconnect"),
                             ToastArguments(L"reconnect", id),
                             L"ms-appx:///Images/ToastConnected.png",
                             L"<audio src=\"ms-winsoundevent:Notification.Default\"/>",
                             L"long");

    auto fallbackBody = ReplacePlaceholders(std::wstring(_("Notification_Connected")), deviceName);
    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::minutes(1)),
                              std::wstring(_("AppName")),
                              fallbackBody,
                              FallbackNotificationType::Info);
}

void NotificationService::ShowDeviceDisconnected(winrt::hstring const&, winrt::hstring const& deviceName) {
    auto title = NotificationText("Notification_Disconnected", deviceName);
    auto xml = BuildToastXml(title,
                             NotificationText("Notification_Disconnected_Body"),
                             L"",
                             L"",
                             L"",
                             L"ms-appx:///Images/ToastWarning.png",
                             L"<audio silent=\"true\"/>");

    auto fallbackBody = ReplacePlaceholders(std::wstring(_("Notification_Disconnected")), deviceName);
    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::minutes(1)),
                              std::wstring(_("AppName")),
                              fallbackBody,
                              FallbackNotificationType::Warning);
}

void NotificationService::ShowAutoReconnect(winrt::hstring const&, winrt::hstring const& deviceName) {
    auto title = NotificationText("Notification_AutoReconnect", deviceName);
    auto xml = BuildToastXml(title,
                             NotificationText("Notification_AutoReconnect_Body"),
                             L"",
                             L"",
                             L"",
                             L"ms-appx:///Images/ToastReconnect.png",
                             L"<audio silent=\"true\"/>");

    auto fallbackBody = ReplacePlaceholders(std::wstring(_("Notification_AutoReconnect")), deviceName);
    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::minutes(1)),
                              std::wstring(_("AppName")),
                              fallbackBody,
                              FallbackNotificationType::Info);
}

void NotificationService::ShowAutoReconnectFailed(winrt::hstring const& id, winrt::hstring const& deviceName) {
    auto title = NotificationText("Notification_AutoReconnectFailed_Title", deviceName);
    auto xml = BuildToastXml(title,
                             NotificationText("Notification_AutoReconnectFailed_Body"),
                             L"",
                             NotificationText("Notification_Retry"),
                             ToastArguments(L"retry", id),
                             L"ms-appx:///Images/ToastError.png",
                             L"<audio src=\"ms-winsoundevent:Notification.Looping.Alarm2\"/>");

    auto fallbackBody = ReplacePlaceholders(std::wstring(_("Notification_AutoReconnectFailed")), deviceName);
    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::hours(1)),
                              std::wstring(_("AppName")),
                              fallbackBody,
                              FallbackNotificationType::Error);
}

void NotificationService::ShowUpdateAvailable(std::wstring const& latestVersion) {
    auto title = NotificationText("Notification_UpdateAvailable_Title", latestVersion);
    auto body = NotificationText("Notification_UpdateAvailable_Body");
    auto xml = BuildToastXml(title,
                             body,
                             L"",
                             NotificationText("Notification_UpdateAvailable_Action"),
                             ToastArguments(L"openUpdate"),
                             L"ms-appx:///Images/ToastInfo.png",
                             L"<audio silent=\"true\"/>");

    ShowStatusToastOrFallback(xml,
                              ExpirationFromNow(std::chrono::hours(6)),
                              std::wstring(_("AppName")),
                              title,
                              FallbackNotificationType::Info);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handler /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void NotificationService::OnNotificationInvoked(AppNotifications::AppNotificationActivatedEventArgs const& args) {
    try {
        ReconnectRequestedCallback reconnectCallback;
        {
            auto guard = m_lock.lock_shared();
            if (m_isTearingDown) return;
            reconnectCallback = m_reconnectCallback;
        }

        auto parsedArguments = ParseToastArgumentString(args.Argument());
        auto action = FindToastArgument(parsedArguments, L"action");
        auto deviceId = FindToastArgument(parsedArguments, L"deviceId");

        if (action && *action == L"openUpdate") {
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

        if (action && (*action == L"reconnect" || *action == L"retry") && reconnectCallback) {
            reconnectCallback(winrt::hstring(*deviceId));
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[NotificationService] App notification activation failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[NotificationService] App notification activation failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[NotificationService] App notification activation failed");
    }
}
