#include <pch.h>
#include <services/UpdateService.hpp>

namespace {
constexpr std::wstring_view c_latestReleaseApiUrl =
    L"https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_latestReleasePageUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_appInstallerUrl =
    L"https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller";
constexpr std::wstring_view c_cachedAppInstallerFileName = L"AudioPlaybackConnector2.appinstaller";
constexpr auto c_updateCheckTimeout = std::chrono::seconds{10};
constexpr size_t c_maxReleaseJsonCharacters = 128 * 1024;
constexpr uint64_t c_maxAppInstallerBytes = 1024 * 1024;
std::atomic_bool g_appInstallerLaunchInProgress = false;

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
    while (start <= value.size()) {
        if (index == version.Parts.size()) return {};

        const auto separator = value.find(L'.', start);
        const auto end = separator == std::wstring_view::npos ? value.size() : separator;
        const auto part = value.substr(start, end - start);
        if (part.empty()) return {};

        uint64_t parsed = 0;
        for (auto ch : part) {
            if (ch < L'0' || ch > L'9') return {};
            const auto digit = static_cast<uint64_t>(ch - L'0');
            if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) return {};
            parsed = (parsed * 10) + digit;
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

bool IsTrustedAppInstallerUri(std::wstring_view value) {
    try {
        winrt::Windows::Foundation::Uri uri{winrt::hstring(value)};
        return uri.SchemeName() == L"https" && _wcsicmp(uri.Host().c_str(), L"n0ahtm.github.io") == 0 &&
               uri.Path().starts_with(L"/AudioPlaybackConnector2/");
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] App Installer URL validation failed", ex);
        return false;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] App Installer URL validation failed", ex);
        return false;
    }
}

template <typename AsyncOperation>
winrt::Windows::System::Threading::ThreadPoolTimer CreateOperationTimeout(AsyncOperation operation,
                                                                          std::shared_ptr<std::atomic<bool>> timedOut) {
    return winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [operation = std::move(operation), timedOut](auto) mutable {
            timedOut->store(true);
            try {
                operation.Cancel();
            } catch (...) {
            }
        },
        c_updateCheckTimeout);
}

void CancelOperationTimeout(winrt::Windows::System::Threading::ThreadPoolTimer const& timer) noexcept {
    if (!timer) return;
    try {
        timer.Cancel();
    } catch (...) {
    }
}

void ValidateAppInstallerXml(winrt::hstring const& xml) {
    winrt::Windows::Data::Xml::Dom::XmlDocument document;
    document.LoadXml(xml);

    auto root = document.DocumentElement();
    if (!root || root.NodeName() != L"AppInstaller") {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer feed root is invalid"));
    }

    auto feedUri = std::wstring(root.GetAttribute(L"Uri"));
    if (!feedUri.empty() && !IsTrustedAppInstallerUri(feedUri)) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer feed URI is not trusted"));
    }

    auto packageNodes = root.GetElementsByTagName(L"MainPackage");
    if (packageNodes.Length() == 0) {
        packageNodes = root.GetElementsByTagName(L"MainBundle");
    }
    if (packageNodes.Length() == 0) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer feed has no main package"));
    }

    auto packageElement = packageNodes.Item(0).as<winrt::Windows::Data::Xml::Dom::XmlElement>();
    auto packageId = winrt::Windows::ApplicationModel::Package::Current().Id();
    if (packageElement.GetAttribute(L"Name") != packageId.Name() ||
        packageElement.GetAttribute(L"Publisher") != packageId.Publisher()) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer package identity is invalid"));
    }

    auto packageUri = std::wstring(packageElement.GetAttribute(L"Uri"));
    if (packageUri.empty() || !IsTrustedAppInstallerUri(packageUri)) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer package URI is not trusted"));
    }
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFile> DownloadAppInstallerFileAsync() {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"AudioPlaybackConnector2");
    client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/appinstaller, application/xml, text/xml, */*");

    auto requestTimedOut = std::make_shared<std::atomic<bool>>(false);
    auto request = client.GetAsync(winrt::Windows::Foundation::Uri(winrt::hstring(c_appInstallerUrl)));
    auto requestTimer = CreateOperationTimeout(request, requestTimedOut);
    auto requestTimerGuard = wil::scope_exit([&]() noexcept { CancelOperationTimeout(requestTimer); });
    auto response = co_await request;
    requestTimerGuard.release();
    CancelOperationTimeout(requestTimer);
    if (!response.IsSuccessStatusCode()) {
        auto message =
            std::format(L"App Installer feed returned HTTP {}", static_cast<uint32_t>(response.StatusCode()));
        throw winrt::hresult_error(E_FAIL, winrt::hstring(message));
    }

    auto content = response.Content();
    auto headers = content.Headers();
    if (headers.ContentLength() && headers.ContentLength().Value() > c_maxAppInstallerBytes) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer feed was too large"));
    }

    auto readTimedOut = std::make_shared<std::atomic<bool>>(false);
    auto read = content.ReadAsStringAsync();
    auto readTimer = CreateOperationTimeout(read, readTimedOut);
    auto readTimerGuard = wil::scope_exit([&]() noexcept { CancelOperationTimeout(readTimer); });
    auto xml = co_await read;
    readTimerGuard.release();
    CancelOperationTimeout(readTimer);
    if (xml.size() > c_maxAppInstallerBytes) {
        throw winrt::hresult_error(E_FAIL, winrt::hstring(L"App Installer feed was too large"));
    }
    ValidateAppInstallerXml(xml);

    auto file = co_await winrt::Windows::Storage::ApplicationData::Current().TemporaryFolder().CreateFileAsync(
        winrt::hstring(c_cachedAppInstallerFileName),
        winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);
    co_await winrt::Windows::Storage::FileIO::WriteTextAsync(file, xml);
    co_return file;
}
} // namespace

UpdateCheckTask UpdateService::CheckForUpdatesAsync(std::stop_token stopToken) {
    UpdateCheckResult result;
    result.CurrentVersion = CurrentVersionString();
    result.ReleaseUrl = std::wstring(c_latestReleasePageUrl);
    result.AppInstallerUrl = std::wstring(c_appInstallerUrl);
    auto requestTimedOut = std::make_shared<std::atomic<bool>>(false);

    try {
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
            co_return result;
        }
        co_await winrt::resume_background();
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
            co_return result;
        }

        winrt::Windows::Web::Http::HttpClient client;
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"AudioPlaybackConnector2");
        client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");

        auto request = client.GetAsync(winrt::Windows::Foundation::Uri(winrt::hstring(c_latestReleaseApiUrl)));
        std::stop_callback requestCancellation(stopToken, [request]() noexcept {
            try {
                request.Cancel();
            } catch (...) {
            }
        });
        auto timeoutTimer = CreateOperationTimeout(request, requestTimedOut);
        auto timeoutGuard = wil::scope_exit([&]() noexcept { CancelOperationTimeout(timeoutTimer); });
        auto response = co_await request;
        timeoutGuard.release();
        CancelOperationTimeout(timeoutTimer);

        if (!response.IsSuccessStatusCode()) {
            result.ErrorMessage = std::format(L"GitHub returned HTTP {}", static_cast<uint32_t>(response.StatusCode()));
            co_return result;
        }

        auto content = response.Content();
        auto headers = content.Headers();
        if (headers.ContentLength() && headers.ContentLength().Value() > c_maxReleaseJsonCharacters) {
            result.ErrorMessage = L"GitHub release response was too large";
            co_return result;
        }
        auto bodyRead = content.ReadAsStringAsync();
        std::stop_callback bodyReadCancellation(stopToken, [bodyRead]() noexcept {
            try {
                bodyRead.Cancel();
            } catch (...) {
            }
        });
        auto bodyTimeoutTimer = CreateOperationTimeout(bodyRead, requestTimedOut);
        auto bodyTimeoutGuard = wil::scope_exit([&]() noexcept { CancelOperationTimeout(bodyTimeoutTimer); });
        auto body = co_await bodyRead;
        bodyTimeoutGuard.release();
        CancelOperationTimeout(bodyTimeoutTimer);
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
            co_return result;
        }
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
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
        } else {
            result.ErrorMessage =
                requestTimedOut->load()
                    ? L"Update check timed out"
                    : std::format(L"0x{:08X} {}", static_cast<uint32_t>(ex.code()), std::wstring(ex.message()));
        }
    } catch (std::exception const& ex) {
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
        } else {
            result.ErrorMessage = util::Utf8ToUtf16(ex.what());
        }
    } catch (...) {
        if (stopToken.stop_requested()) {
            result.Status = UpdateCheckStatus::Cancelled;
            result.ErrorMessage = L"Update check cancelled";
        } else {
            result.ErrorMessage = L"Unknown update check error";
        }
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
    bool expected = false;
    if (!g_appInstallerLaunchInProgress.compare_exchange_strong(expected, true)) {
        DebugTrace(L"[UpdateService] App Installer launch already in progress");
        co_return;
    }
    auto launchGuard = wil::scope_exit([]() noexcept { g_appInstallerLaunchInProgress = false; });

    try {
        auto file = co_await DownloadAppInstallerFileAsync();
        if (co_await winrt::Windows::System::Launcher::LaunchFileAsync(file)) {
            co_return;
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to download or launch App Installer file", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[UpdateService] Failed to download or launch App Installer file", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[UpdateService] Failed to download or launch App Installer file");
    }

    LaunchReleasePageAsync(std::wstring(c_latestReleasePageUrl));
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
