#include <pch.h>
#include <services/UpdateService.hpp>

namespace {
constexpr std::wstring_view c_latestReleaseApiUrl =
    L"https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_latestReleasePageUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest";
constexpr std::wstring_view c_appInstallerUrl =
    L"https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller";

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
} // namespace

UpdateCheckResult UpdateService::CheckForUpdates() {
    UpdateCheckResult result;
    result.CurrentVersion = CurrentVersionString();
    result.ReleaseUrl = std::wstring(c_latestReleasePageUrl);
    result.AppInstallerUrl = std::wstring(c_appInstallerUrl);

    try {
        winrt::Windows::Web::Http::HttpClient client;
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"AudioPlaybackConnector2");
        client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");

        auto response = client.GetAsync(winrt::Windows::Foundation::Uri(winrt::hstring(c_latestReleaseApiUrl))).get();
        if (!response.IsSuccessStatusCode()) {
            result.ErrorMessage = std::format(L"GitHub returned HTTP {}", static_cast<uint32_t>(response.StatusCode()));
            return result;
        }

        auto body = response.Content().ReadAsStringAsync().get();
        auto json = winrt::Windows::Data::Json::JsonObject::Parse(body);
        auto tagName = ReadString(json, L"tag_name");
        if (tagName.empty()) {
            result.ErrorMessage = L"GitHub release response did not contain tag_name";
            return result;
        }

        if (auto releaseUrl = ReadString(json, L"html_url"); !releaseUrl.empty()) {
            result.ReleaseUrl = std::move(releaseUrl);
        }

        auto latestVersion = ParseVersion(tagName);
        if (!latestVersion.Valid) {
            result.ErrorMessage = std::format(L"Unsupported release tag: {}", tagName);
            return result;
        }

        auto currentVersion = ParsePackageVersion(winrt::Windows::ApplicationModel::Package::Current().Id().Version());
        result.LatestVersion = FormatVersion(latestVersion);
        result.Status = CompareVersions(latestVersion, currentVersion) > 0 ? UpdateCheckStatus::UpdateAvailable
                                                                           : UpdateCheckStatus::UpToDate;
        return result;
    } catch (winrt::hresult_error const& ex) {
        result.ErrorMessage = std::format(L"0x{:08X} {}", static_cast<uint32_t>(ex.code()), std::wstring(ex.message()));
    } catch (std::exception const& ex) {
        result.ErrorMessage = util::Utf8ToUtf16(ex.what());
    } catch (...) {
        result.ErrorMessage = L"Unknown update check error";
    }

    return result;
}

std::wstring UpdateService::CurrentVersionString() {
    try {
        auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        return FormatVersion(ParsePackageVersion(version));
    } catch (...) {
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
        DebugTrace(L"[UpdateService] Failed to launch App Installer protocol URI: 0x{0:X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
    } catch (...) {
        DebugTrace(L"[UpdateService] Failed to launch App Installer protocol URI: unknown exception");
    }

    try {
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(winrt::hstring(c_appInstallerUrl)));
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[UpdateService] Failed to launch appinstaller URI: 0x{0:X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
    } catch (...) {
        DebugTrace(L"[UpdateService] Failed to launch appinstaller URI: unknown exception");
    }
}

winrt::fire_and_forget UpdateService::LaunchReleasePageAsync(std::wstring releaseUrl) {
    try {
        co_await winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(winrt::hstring(std::move(releaseUrl))));
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[UpdateService] Failed to launch release URI: 0x{0:X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
    } catch (...) {
        DebugTrace(L"[UpdateService] Failed to launch release URI: unknown exception");
    }
}
