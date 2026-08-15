#include <pch.h>

#include <ui/SettingsDiagnosticsReport.hpp>

#include <core/StringResources.hpp>
#include <util/Util.hpp>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.Profile.h>

#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace {
std::wstring RedactedDeviceDisplay(DeviceSettings const& device) {
    (void)device;
    return std::wstring(_("Privacy_RedactedDevice"));
}

void ReplaceAll(std::wstring& value, std::wstring_view needle, std::wstring_view replacement) {
    if (needle.empty()) return;

    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::wstring::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::wstring EnvironmentValue(std::wstring_view name) {
    std::wstring buffer(32767, L'\0');
    auto length = GetEnvironmentVariableW(std::wstring(name).c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::wstring RedactKnownPaths(std::wstring value) {
    auto localAppData = EnvironmentValue(L"LOCALAPPDATA");
    if (!localAppData.empty()) ReplaceAll(value, localAppData, L"%LOCALAPPDATA%");

    auto userProfile = EnvironmentValue(L"USERPROFILE");
    if (!userProfile.empty()) ReplaceAll(value, userProfile, L"%USERPROFILE%");
    return value;
}

std::wstring RedactSensitivePatterns(std::wstring value) {
    static std::wregex const macPattern(LR"((?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})");
    static std::wregex const bluetoothDevicePattern(LR"((BTHENUM\\DEV_)[0-9A-Fa-f]{12,})");
    static std::wregex const guidPattern(
        LR"(\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}?)");

    value = std::regex_replace(value, macPattern, std::wstring(_("Privacy_RedactedValue")));
    value = std::regex_replace(
        value, bluetoothDevicePattern, std::wstring(L"$1") + std::wstring(_("Privacy_RedactedValue")));
    value = std::regex_replace(value, guidPattern, std::wstring(_("Privacy_RedactedValue")));
    return value;
}

std::wstring RedactDiagnosticLine(std::wstring line, SettingsData const& settings) {
    line = RedactKnownPaths(std::move(line));
    for (auto const& device : settings.Devices) {
        ReplaceAll(line, device.Id, _("Privacy_RedactedValue"));
        if (!device.Name.empty()) ReplaceAll(line, device.Name, RedactedDeviceDisplay(device));
        if (!device.Alias.empty()) ReplaceAll(line, device.Alias, RedactedDeviceDisplay(device));
    }
    return RedactSensitivePatterns(std::move(line));
}

std::wstring DefaultDeviceModeText(DefaultDeviceMode mode) {
    return mode == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
}

std::wstring ToLowerInvariant(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value)
        result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return result;
}

std::vector<std::wstring>
RecentRelevantLogLines(std::filesystem::path const& path, std::size_t maxLines, SettingsData const& settings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto text = util::Utf8ToUtf16(bytes);
    std::vector<std::wstring> matches;
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        auto lower = ToLowerInvariant(line);
        if (lower.find(L"error") == std::wstring::npos && lower.find(L"failed") == std::wstring::npos &&
            lower.find(L"warning") == std::wstring::npos && lower.find(L"crash") == std::wstring::npos) {
            continue;
        }
        matches.push_back(RedactDiagnosticLine(std::move(line), settings));
        if (matches.size() > maxLines) matches.erase(matches.begin());
    }
    return matches;
}
} // namespace

std::wstring apc::ui::BuildSettingsDiagnosticsReport(SettingsData const& settings,
                                                     std::size_t connectedDeviceCount,
                                                     std::filesystem::path const& logPath,
                                                     std::wstring_view appVersionText) {
    std::wstringstream output;
    std::wstring packageIdentity = _("Settings_Diagnostics_Unpackaged");
    std::wstring installType = _("Settings_Diagnostics_Unpackaged");
    try {
        packageIdentity = std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().FullName());
        installType = _("Settings_Diagnostics_Packaged");
    } catch (...) {
    }

    std::wstring windowsVersion = _("Settings_Diagnostics_Unknown");
    try {
        windowsVersion =
            std::wstring(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion());
    } catch (...) {
    }

    output << _("Settings_Diagnostics_Title") << L"\n";
    output << _("Settings_Diagnostics_AppVersion") << L": " << appVersionText << L"\n";
    output << _("Settings_Diagnostics_WindowsVersion") << L": " << windowsVersion << L"\n";
    output << _("Settings_Diagnostics_PackageIdentity") << L": " << packageIdentity << L"\n";
    output << _("Settings_Diagnostics_InstallType") << L": " << installType << L"\n";
    output << _("Settings_Diagnostics_PrivacyMode") << L": "
           << (settings.PrivacyModeEnabled ? _("Settings_On") : _("Settings_Off")) << L"\n";
    output << _("Settings_Diagnostics_ConnectedCount") << L": " << connectedDeviceCount << L"\n";
    output << _("Settings_Diagnostics_KnownDevices") << L": " << settings.Devices.size() << L"\n";
    output << _("Settings_Diagnostics_DefaultMode") << L": " << DefaultDeviceModeText(settings.DefaultDevice) << L"\n";
    output << _("Settings_Diagnostics_LogPath") << L": " << RedactKnownPaths(logPath.wstring()) << L"\n";
    output << L"\n" << _("Settings_Diagnostics_Devices") << L":\n";

    if (settings.Devices.empty()) {
        output << L"- " << _("Settings_NoDevices") << L"\n";
    } else {
        for (auto const& device : settings.Devices) {
            output << L"- " << RedactedDeviceDisplay(device);
            if (settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == device.Id) {
                output << L" (" << _("Settings_DefaultDevice_Current") << L")";
            }
            output << L"\n";
            output << L"  " << _("Settings_Diagnostics_DeviceId") << L": " << _("Privacy_RedactedValue") << L"\n";
            output << L"  " << _("Settings_Diagnostics_Alias") << L": "
                   << (device.Alias.empty() ? _("Command_AliasNone") : _("Privacy_RedactedValue")) << L"\n";
            output << L"  " << _("Settings_Diagnostics_ConnectOnStartup") << L": "
                   << (device.ConnectOnStartup ? _("Settings_On") : _("Settings_Off")) << L"\n";
            output << L"  " << _("Settings_Diagnostics_ReconnectOnConnectionLoss") << L": "
                   << (device.ReconnectOnConnectionLoss ? _("Settings_On") : _("Settings_Off")) << L"\n";
        }
    }

    auto recentErrors = RecentRelevantLogLines(logPath, 5, settings);
    output << L"\n" << _("Settings_Diagnostics_RecentErrors") << L":\n";
    if (recentErrors.empty()) {
        output << L"- " << _("Settings_Diagnostics_NoRecentErrors") << L"\n";
    } else {
        for (auto const& line : recentErrors)
            output << L"- " << line << L"\n";
    }

    output << L"\n" << _("Settings_Diagnostics_PrivacyNote") << L"\n";
    output << _("Settings_Diagnostics_DumpWarning") << L"\n";
    return output.str();
}
