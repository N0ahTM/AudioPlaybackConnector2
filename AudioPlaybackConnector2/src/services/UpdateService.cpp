#include <pch.h>
#include <services/UpdateService.hpp>

namespace {
constexpr std::wstring_view c_latestReleaseApiUrl =
    L"https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_latestReleasePageUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_appInstallerUrl =
    L"https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller";
constexpr auto c_updateCheckTimeout = std::chrono::seconds{10};
constexpr size_t c_maxReleaseJsonCharacters = 128 * 1024;

struct ParsedVersion {
    std::array<uint64_t, 4> Parts{};
    bool Valid = false;
};

ParsedVersion ParseVersion(std::wstring_view value) {
    ParsedVersion version;
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) {
        value.remove_prefix(1);
    }

    if (auto suffix = value.find_first_of(L"-+"); suffix != std::wstring_view::npos) {
        value = value.substr(0, suffix);
    }

    size_t index = 0;
    size_t start = 0;
    while (start <= value.size() && index < version.Parts.size()) {
        const auto separator = value.find(L'.', start);
        const auto end = separator == std::wstring_view::npos ? value.size() : separator;
        const auto part = value.substr(start, end - start);
        if (part.empty()) return {};

        uint64_t parsed = 0;
        for (auto ch : part) {
            if (ch < L'0' || ch > L'9') return {};
            parsed = (parsed * 10) + static_cast<uint64_t>(ch - L'0');
        }
        version.Parts[index++] = parsed;

        if (separator == std::wstring_view::npos) break;
        start = separator + 1;
    }

    version.Valid = index >= 2;
    return version;
}

ParsedVersion ParsePackageVersion(winrt::Windows::ApplicationModel::PackageVersion const& packageVersion) {
    ParsedVersion version;
    version.Parts = {packageVersion.Major, packageVersion.Minor, packageVersion.Build, packageVersion.Revision};
    version.Valid = true;
    return version;
}

int CompareVersions(ParsedVersion const& left, ParsedVersion const& right) {
    for (size_t i = 0; i < left.Parts.size(); ++i) {
        if (left.Parts[i] > right.Parts[i]) return 1;
        if (left.Parts[i] < right.Parts[i]) return -1;
    }
    return 0;
}

std::wstring FormatVersion(ParsedVersion const& version) {
    if (!version.Valid) return {};
    if (version.Parts[3] == 0) {
        return std::format(L"{}.{}.{}", version.Parts[0], version.Parts[1], version.Parts[2]);
    }
    return std::format(L"{}.{}.{}.{}", version.Parts[0], version.Parts[1], version.Parts[2], version.Parts[3]);
}

std::wstring ReadString(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key) {
    if (!json.HasKey(key)) return {};
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String ? std::wstring(value.GetString())
                                                                                  : std::wstring{};
}

bool ReadBoolean(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, bool fallback = false) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean ? value.GetBoolean() : fallback;
}

bool IsTrustedReleasePageUrl(std::wstring_view value) {
    try {
        winrt::Windows::Foundation::Uri uri{winrt::hstring(value)};
        return uri.SchemeName() == L"https" && _wcsicmp(uri.Host().c_str(), L"github.com") == 0;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] Release URL validation failed", ex);
        return false;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] Release URL validation failed", ex);
        return false;
    }
}
} // namespace

UpdateCheckTask UpdateCheckTask::promise_type::get_return_object() noexcept {
    return UpdateCheckTask{UpdateCheckTask::Handle::from_promise(*this)};
}

auto UpdateCheckTask::promise_type::final_suspend() noexcept {
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        void await_suspend(UpdateCheckTask::Handle handle) noexcept {
            if (auto continuation = handle.promise().Continuation) {
                continuation.resume();
            }
        }

        void await_resume() noexcept {}
    };

    return FinalAwaiter{};
}

void UpdateCheckTask::await_suspend(std::coroutine_handle<> continuation) noexcept {
    m_handle.promise().Continuation = continuation;
    m_handle.resume();
}

UpdateCheckResult UpdateCheckTask::await_resume() {
    if (m_handle.promise().Exception) {
        std::rethrow_exception(m_handle.promise().Exception);
    }
    return std::move(m_handle.promise().Result);
}

UpdateCheckTask UpdateService::CheckForUpdatesAsync() {
    UpdateCheckResult result;
    result.CurrentVersion = CurrentVersionString();
    result.ReleaseUrl = std::wstring(c_latestReleasePageUrl);
    result.AppInstallerUrl = std::wstring(c_appInstallerUrl);
    auto requestTimedOut = std::make_shared<std::atomic<bool>>(false);

    try {
        co_await winrt::resume_background();

        winrt::Windows::Web::Http::HttpClient client;
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"AudioPlaybackConnector2");
        client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");

        auto request = client.GetAsync(winrt::Windows::Foundation::Uri(winrt::hstring(c_latestReleaseApiUrl)));
        auto timeoutTimer = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [request, requestTimedOut](auto) mutable {
                requestTimedOut->store(true);
                try {
                    request.Cancel();
                } catch (...) {
                }
            },
            c_updateCheckTimeout);
        auto timeoutGuard = wil::scope_exit([&]() noexcept {
            try {
                timeoutTimer.Cancel();
            } catch (...) {
            }
        });

        auto response = co_await request;
        timeoutGuard.release();
        try {
            timeoutTimer.Cancel();
        } catch (...) {
        }

        if (!response.IsSuccessStatusCode()) {
            result.ErrorMessage = std::format(L"GitHub returned HTTP {}", static_cast<uint32_t>(response.StatusCode()));
            co_return result;
        }

        auto body = co_await response.Content().ReadAsStringAsync();
        if (body.size() > c_maxReleaseJsonCharacters) {
            result.ErrorMessage = L"GitHub release response was too large";
            co_return result;
        }

        auto json = winrt::Windows::Data::Json::JsonObject::Parse(body);
        if (ReadBoolean(json, L"draft") || ReadBoolean(json, L"prerelease")) {
            result.ErrorMessage = L"GitHub release response points to a draft or prerelease";
            co_return result;
        }

        auto tagName = ReadString(json, L"tag_name");
        if (tagName.empty()) {
            result.ErrorMessage = L"GitHub release response did not contain tag_name";
            co_return result;
        }

        if (auto releaseUrl = ReadString(json, L"html_url"); !releaseUrl.empty()) {
            if (IsTrustedReleasePageUrl(releaseUrl)) {
                result.ReleaseUrl = std::move(releaseUrl);
            } else {
                DebugTrace(L"[UpdateService] Ignoring untrusted release URL: {0}", releaseUrl);
            }
        }

        auto latestVersion = ParseVersion(tagName);
        if (!latestVersion.Valid) {
            result.ErrorMessage = std::format(L"Unsupported release tag: {}", tagName);
            co_return result;
        }

        auto currentVersion = ParsePackageVersion(winrt::Windows::ApplicationModel::Package::Current().Id().Version());
        result.LatestVersion = FormatVersion(latestVersion);
        result.Status = CompareVersions(latestVersion, currentVersion) > 0 ? UpdateCheckStatus::UpdateAvailable
                                                                           : UpdateCheckStatus::UpToDate;
        co_return result;
    } catch (winrt::hresult_error const& ex) {
        result.ErrorMessage =
            requestTimedOut->load()
                ? L"Update check timed out"
                : std::format(L"0x{:08X} {}", static_cast<uint32_t>(ex.code()), std::wstring(ex.message()));
    } catch (std::exception const& ex) {
        result.ErrorMessage = util::Utf8ToUtf16(ex.what());
    } catch (...) {
        result.ErrorMessage = L"Unknown update check error";
    }

    co_return result;
}

std::wstring UpdateService::CurrentVersionString() {
    try {
        auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        return FormatVersion(ParsePackageVersion(version));
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] CurrentVersionString failed", ex);
        return {};
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] CurrentVersionString failed", ex);
        return {};
    }
}

std::wstring_view UpdateService::AppInstallerUrl() {
    return c_appInstallerUrl;
}

std::wstring_view UpdateService::LatestReleaseApiUrl() {
    return c_latestReleaseApiUrl;
}

std::wstring_view UpdateService::LatestReleasePageUrl() {
    return c_latestReleasePageUrl;
}

winrt::fire_and_forget UpdateService::LaunchAppInstallerAsync() {
    try {
        const auto escapedUrl = winrt::Windows::Foundation::Uri::EscapeComponent(winrt::hstring(c_appInstallerUrl));
        std::wstring protocolUri = L"ms-appinstaller:?source=";
        protocolUri += escapedUrl.c_str();

        if (co_await winrt::Windows::System::Launcher::LaunchUriAsync(
                winrt::Windows::Foundation::Uri(winrt::hstring(protocolUri)))) {
            co_return;
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch App Installer protocol URI", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch App Installer protocol URI", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[UpdateService] Failed to launch App Installer protocol URI");
    }

    try {
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(winrt::hstring(c_appInstallerUrl)));
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch appinstaller URI", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch appinstaller URI", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[UpdateService] Failed to launch appinstaller URI");
    }
}

winrt::fire_and_forget UpdateService::LaunchReleasePageAsync(std::wstring releaseUrl) {
    try {
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(winrt::hstring(std::move(releaseUrl))));
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch release URI", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to launch release URI", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[UpdateService] Failed to launch release URI");
    }
}
