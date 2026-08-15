#include <ui/SettingsDiagnosticsReport.hpp>

#include <sstream>

namespace {
std::wstring_view DefaultDeviceModeText(DefaultDeviceMode mode) noexcept {
    return mode == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
}
} // namespace

std::wstring apc::ui::BuildSettingsDiagnosticsReport(SettingsData const& settings,
                                                     std::size_t connectedDeviceCount,
                                                     SettingsDiagnosticsReportContext const& context,
                                                     DiagnosticsLogResult const& logResult) {
    std::wstringstream output;
    output << context.Title << L"\n";
    output << context.AppVersionLabel << L": " << context.AppVersionText << L"\n";
    output << context.WindowsVersionLabel << L": " << context.WindowsVersion << L"\n";
    output << context.PackageIdentityLabel << L": " << context.PackageIdentity << L"\n";
    output << context.InstallTypeLabel << L": " << context.InstallType << L"\n";
    output << context.PrivacyModeLabel << L": " << (settings.PrivacyModeEnabled ? context.On : context.Off) << L"\n";
    output << context.ConnectedCountLabel << L": " << connectedDeviceCount << L"\n";
    output << context.KnownDevicesLabel << L": " << settings.Devices.size() << L"\n";
    output << context.DefaultModeLabel << L": " << DefaultDeviceModeText(settings.DefaultDevice) << L"\n";
    output << context.LogPathLabel << L": " << logResult.DisplayPath << L"\n";
    output << L"\n" << context.DevicesLabel << L":\n";

    if (settings.Devices.empty()) {
        output << L"- " << context.NoDevices << L"\n";
    } else {
        for (auto const& device : settings.Devices) {
            output << L"- " << context.RedactedDevice;
            if (settings.DefaultDevice == DefaultDeviceMode::SpecificDevice && settings.DefaultDeviceId == device.Id) {
                output << L" (" << context.DefaultDeviceCurrent << L")";
            }
            output << L"\n";
            output << L"  " << context.DeviceIdLabel << L": " << context.RedactedValue << L"\n";
            output << L"  " << context.AliasLabel << L": "
                   << (device.Alias.empty() ? context.AliasNone : context.RedactedValue) << L"\n";
            output << L"  " << context.ConnectOnStartupLabel << L": "
                   << (device.ConnectOnStartup ? context.On : context.Off) << L"\n";
            output << L"  " << context.ReconnectOnConnectionLossLabel << L": "
                   << (device.ReconnectOnConnectionLoss ? context.On : context.Off) << L"\n";
        }
    }

    output << L"\n" << context.RecentErrorsLabel << L":\n";
    if (logResult.Status == DiagnosticsLogStatus::Unavailable) {
        output << L"- " << context.LogUnavailable << L"\n";
    } else if (logResult.Lines.empty() && !logResult.SkippedMalformedOrOversized) {
        output << L"- " << context.NoRecentErrors << L"\n";
    } else {
        for (auto const& line : logResult.Lines)
            output << L"- " << line << L"\n";
    }
    if (logResult.SkippedMalformedOrOversized) {
        output << L"- " << context.LogEntriesOmitted << L"\n";
    }

    output << L"\n" << context.PrivacyNote << L"\n";
    output << context.DumpWarning << L"\n";
    auto report = output.str();
    return report.size() <= c_settingsDiagnosticsReportMaxCharacters ? std::move(report) : std::wstring{};
}
