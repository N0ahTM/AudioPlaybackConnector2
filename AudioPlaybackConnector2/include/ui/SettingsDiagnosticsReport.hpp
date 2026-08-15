#pragma once

#include <core/SettingsData.hpp>
#include <ui/DiagnosticsLogCollector.hpp>

#include <string>
#include <string_view>

namespace apc::ui {
inline constexpr std::size_t c_settingsDiagnosticsReportMaxCharacters = 128 * 1024;

struct SettingsDiagnosticsReportContext {
    std::wstring AppVersionText;
    std::wstring PackageIdentity;
    std::wstring InstallType;
    std::wstring WindowsVersion;
    std::wstring Title;
    std::wstring AppVersionLabel;
    std::wstring WindowsVersionLabel;
    std::wstring PackageIdentityLabel;
    std::wstring InstallTypeLabel;
    std::wstring PrivacyModeLabel;
    std::wstring On;
    std::wstring Off;
    std::wstring ConnectedCountLabel;
    std::wstring KnownDevicesLabel;
    std::wstring DefaultModeLabel;
    std::wstring LogPathLabel;
    std::wstring DevicesLabel;
    std::wstring NoDevices;
    std::wstring RedactedDevice;
    std::wstring DefaultDeviceCurrent;
    std::wstring DeviceIdLabel;
    std::wstring RedactedValue;
    std::wstring AliasLabel;
    std::wstring AliasNone;
    std::wstring ConnectOnStartupLabel;
    std::wstring ReconnectOnConnectionLossLabel;
    std::wstring RecentErrorsLabel;
    std::wstring LogUnavailable;
    std::wstring NoRecentErrors;
    std::wstring LogEntriesOmitted;
    std::wstring PrivacyNote;
    std::wstring DumpWarning;
};

[[nodiscard]] SettingsDiagnosticsReportContext CaptureSettingsDiagnosticsReportContext(std::wstring appVersionText);
[[nodiscard]] std::wstring BuildSettingsDiagnosticsReport(SettingsData const& settings,
                                                          std::size_t connectedDeviceCount,
                                                          SettingsDiagnosticsReportContext const& context,
                                                          DiagnosticsLogResult const& logResult);
} // namespace apc::ui
