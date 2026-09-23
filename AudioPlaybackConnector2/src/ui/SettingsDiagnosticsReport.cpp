#include <pch.h>

#include <ui/SettingsDiagnosticsReport.hpp>

#include <core/StringResources.hpp>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.Profile.h>

apc::ui::SettingsDiagnosticsReportContext
apc::ui::CaptureSettingsDiagnosticsReportContext(std::wstring appVersionText, StringResources const& strings) {
    SettingsDiagnosticsReportContext context;
    context.AppVersionText = std::move(appVersionText);
    context.PackageIdentity = strings.Get("Settings_Diagnostics_Unpackaged");
    context.InstallType = strings.Get("Settings_Diagnostics_Unpackaged");
    try {
        context.PackageIdentity = std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().FullName());
        context.InstallType = strings.Get("Settings_Diagnostics_Packaged");
    } catch (...) {
    }
    context.WindowsVersion = strings.Get("Settings_Diagnostics_Unknown");
    try {
        context.WindowsVersion =
            std::wstring(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion());
    } catch (...) {
    }
    context.Title = strings.Get("Settings_Diagnostics_Title");
    context.AppVersionLabel = strings.Get("Settings_Diagnostics_AppVersion");
    context.WindowsVersionLabel = strings.Get("Settings_Diagnostics_WindowsVersion");
    context.PackageIdentityLabel = strings.Get("Settings_Diagnostics_PackageIdentity");
    context.InstallTypeLabel = strings.Get("Settings_Diagnostics_InstallType");
    context.PrivacyModeLabel = strings.Get("Settings_Diagnostics_PrivacyMode");
    context.On = strings.Get("Settings_On");
    context.Off = strings.Get("Settings_Off");
    context.ConnectedCountLabel = strings.Get("Settings_Diagnostics_ConnectedCount");
    context.KnownDevicesLabel = strings.Get("Settings_Diagnostics_KnownDevices");
    context.DefaultModeLabel = strings.Get("Settings_Diagnostics_DefaultMode");
    context.LogPathLabel = strings.Get("Settings_Diagnostics_LogPath");
    context.DevicesLabel = strings.Get("Settings_Diagnostics_Devices");
    context.NoDevices = strings.Get("Settings_NoDevices");
    context.RedactedDevice = strings.Get("Privacy_RedactedDevice");
    context.DefaultDeviceCurrent = strings.Get("Settings_DefaultDevice_Current");
    context.DeviceIdLabel = strings.Get("Settings_Diagnostics_DeviceId");
    context.RedactedValue = strings.Get("Privacy_RedactedValue");
    context.AliasLabel = strings.Get("Settings_Diagnostics_Alias");
    context.AliasNone = strings.Get("Command_AliasNone");
    context.ConnectOnStartupLabel = strings.Get("Settings_Diagnostics_ConnectOnStartup");
    context.ReconnectOnConnectionLossLabel = strings.Get("Settings_Diagnostics_ReconnectOnConnectionLoss");
    context.RecentErrorsLabel = strings.Get("Settings_Diagnostics_RecentErrors");
    context.LogUnavailable = strings.Get("Settings_Diagnostics_LogUnavailable");
    context.NoRecentErrors = strings.Get("Settings_Diagnostics_NoRecentErrors");
    context.LogEntriesOmitted = strings.Get("Settings_Diagnostics_LogEntriesOmitted");
    context.PrivacyNote = strings.Get("Settings_Diagnostics_PrivacyNote");
    context.DumpWarning = strings.Get("Settings_Diagnostics_DumpWarning");
    return context;
}
