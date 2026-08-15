#include <pch.h>

#include <ui/SettingsDiagnosticsReport.hpp>

#include <core/StringResources.hpp>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.Profile.h>

apc::ui::SettingsDiagnosticsReportContext
apc::ui::CaptureSettingsDiagnosticsReportContext(std::wstring appVersionText) {
    SettingsDiagnosticsReportContext context;
    context.AppVersionText = std::move(appVersionText);
    context.PackageIdentity = _("Settings_Diagnostics_Unpackaged");
    context.InstallType = _("Settings_Diagnostics_Unpackaged");
    try {
        context.PackageIdentity = std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().FullName());
        context.InstallType = _("Settings_Diagnostics_Packaged");
    } catch (...) {
    }
    context.WindowsVersion = _("Settings_Diagnostics_Unknown");
    try {
        context.WindowsVersion =
            std::wstring(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion());
    } catch (...) {
    }
    context.Title = _("Settings_Diagnostics_Title");
    context.AppVersionLabel = _("Settings_Diagnostics_AppVersion");
    context.WindowsVersionLabel = _("Settings_Diagnostics_WindowsVersion");
    context.PackageIdentityLabel = _("Settings_Diagnostics_PackageIdentity");
    context.InstallTypeLabel = _("Settings_Diagnostics_InstallType");
    context.PrivacyModeLabel = _("Settings_Diagnostics_PrivacyMode");
    context.On = _("Settings_On");
    context.Off = _("Settings_Off");
    context.ConnectedCountLabel = _("Settings_Diagnostics_ConnectedCount");
    context.KnownDevicesLabel = _("Settings_Diagnostics_KnownDevices");
    context.DefaultModeLabel = _("Settings_Diagnostics_DefaultMode");
    context.LogPathLabel = _("Settings_Diagnostics_LogPath");
    context.DevicesLabel = _("Settings_Diagnostics_Devices");
    context.NoDevices = _("Settings_NoDevices");
    context.RedactedDevice = _("Privacy_RedactedDevice");
    context.DefaultDeviceCurrent = _("Settings_DefaultDevice_Current");
    context.DeviceIdLabel = _("Settings_Diagnostics_DeviceId");
    context.RedactedValue = _("Privacy_RedactedValue");
    context.AliasLabel = _("Settings_Diagnostics_Alias");
    context.AliasNone = _("Command_AliasNone");
    context.ConnectOnStartupLabel = _("Settings_Diagnostics_ConnectOnStartup");
    context.ReconnectOnConnectionLossLabel = _("Settings_Diagnostics_ReconnectOnConnectionLoss");
    context.RecentErrorsLabel = _("Settings_Diagnostics_RecentErrors");
    context.LogUnavailable = _("Settings_Diagnostics_LogUnavailable");
    context.NoRecentErrors = _("Settings_Diagnostics_NoRecentErrors");
    context.LogEntriesOmitted = _("Settings_Diagnostics_LogEntriesOmitted");
    context.PrivacyNote = _("Settings_Diagnostics_PrivacyNote");
    context.DumpWarning = _("Settings_Diagnostics_DumpWarning");
    return context;
}
