#include <pch.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <ui/SettingsWindow/SettingsWindow.xaml.h>
#if __has_include("SettingsWindow.g.cpp")
#include <SettingsWindow.g.cpp>
#endif

#include <core/SettingsData.hpp>
#include <core/SettingsLimits.hpp>
#include <core/StringResources.hpp>
#include <services/UpdateCoordinator.hpp>
#include <services/UpdateService.hpp>
#include <ui/ButtonHelpers.hpp>
#include <ui/DiagnosticsLogCollector.hpp>
#include <ui/SettingsDiagnosticsReport.hpp>
#include <util/Logger.hpp>
#include <util/Util.hpp>
#include <ui/XamlWindowInterop.hpp>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include <cmath>
#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Windowing;
namespace xaml_automation = winrt::Microsoft::UI::Xaml::Automation;

namespace {
constexpr auto c_placementSaveDelay = std::chrono::seconds(3);
constexpr std::wstring_view c_repositoryUrl = L"https://github.com/N0ahTM/AudioPlaybackConnector2";
constexpr std::wstring_view c_bugReportUrl = L"https://github.com/N0ahTM/AudioPlaybackConnector2/issues/new?labels=bug";
constexpr std::wstring_view c_featureRequestUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/issues/new?labels=enhancement";
constexpr std::wstring_view c_troubleshootingUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/blob/main/docs/TROUBLESHOOTING.md";
constexpr double c_settingsContentMinWidthDip = 420.0;
constexpr double c_settingsContentMaxWidthDip = 520.0;
constexpr double c_settingsContentWidthStepDip = 20.0;
constexpr double c_settingsTitleBarHeightDip = 32.0;
constexpr double c_settingsNavigationHeightDip = 36.0;
constexpr double c_settingsMeasureInfinityDip = 100000.0;
constexpr double c_settingsContentRightSafetyDip = 10.0;

double PixelToDip(int32_t value, UINT dpi) {
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
    return static_cast<double>(value) * static_cast<double>(USER_DEFAULT_SCREEN_DPI) / static_cast<double>(dpi);
}

int32_t DipToPixelCeil(double value, UINT dpi) {
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
    return std::max<int32_t>(1,
                             static_cast<int32_t>(std::ceil(value * static_cast<double>(dpi) /
                                                            static_cast<double>(USER_DEFAULT_SCREEN_DPI))));
}

std::wstring PercentEncode(std::wstring_view value) {
    auto utf8 = util::Utf16ToUtf8(value);
    std::wstring result;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (unsigned char ch : utf8) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<wchar_t>(ch));
        } else if (ch == ' ') {
            result.push_back(L'+');
        } else {
            result.push_back(L'%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0F]);
        }
    }
    return result;
}

void SetAutomationName(DependencyObject const& element, std::wstring_view name) {
    xaml_automation::AutomationProperties::SetName(element, winrt::hstring(name));
}

void SetItemContent(ComboBoxItem const& item, std::wstring_view text) {
    item.Content(winrt::box_value(winrt::hstring(text)));
}

void SetItemContent(NavigationViewItem const& item, std::wstring_view text) {
    item.Content(winrt::box_value(winrt::hstring(text)));
}

std::wstring BuildVersionText() {
    std::wstring label(_("About_Version"));
    try {
        auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        auto versionText =
            version.Revision == 0
                ? std::format(L"{}.{}.{}", version.Major, version.Minor, version.Build)
                : std::format(L"{}.{}.{}.{}", version.Major, version.Minor, version.Build, version.Revision);
        return std::format(L"{} {}", label, versionText);
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(
            L"[SettingsWindow] BuildVersionText failed: 0x{0:08X} {1}", static_cast<uint32_t>(ex.code()), ex.message());
        return label;
    } catch (std::exception const& ex) {
        DebugTrace(L"[SettingsWindow] BuildVersionText failed: {0}", util::Utf8ToUtf16(ex.what()));
        return label;
    } catch (...) {
        DebugTrace(L"[SettingsWindow] BuildVersionText failed: unknown exception");
        return label;
    }
}

} // namespace

namespace winrt::AudioPlaybackConnector2::implementation {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsWindow::SettingsWindow() {
    InitializeComponent();
}

SettingsWindow::~SettingsWindow() {
    StopPageTransition();
    if (m_startupTaskCoordinator && m_startupTaskHandlerToken != 0) {
        m_startupTaskCoordinator->Unsubscribe(std::exchange(m_startupTaskHandlerToken, 0));
    }
    StopPlacementSaveTimer();
    if (m_actualThemeChangedToken.value != 0) {
        try {
            RootGrid().ActualThemeChanged(m_actualThemeChangedToken);
        } catch (...) {
        }
    }
}

LRESULT CALLBACK SettingsWindow::SettingsWindowSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept try {
    auto self = reinterpret_cast<SettingsWindow*>(dwRefData);
    if (msg == WM_GETMINMAXINFO) {
        auto minSize = util::GetSettingsWindowMinTrackSize(hwnd);
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = minSize.cx;
        info->ptMinTrackSize.y = minSize.cy;
        return 0;
    }

    if (self && msg == WM_CLOSE) {
        self->StopPageTransition();
        self->StopPlacementSaveTimer();
        (void)self->StoreCurrentPlacement();
    } else if (self && msg == WM_EXITSIZEMOVE) {
        self->QueuePlacementSave();
    } else if (self && msg == WM_WINDOWPOSCHANGED) {
        self->QueuePlacementSave();
    }

    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, SettingsWindowSubclassProc, uIdSubclass);
        if (self) {
            self->StopPlacementSaveTimer();
            self->m_placementSaveTimer = nullptr;
            if (self->m_actualThemeChangedToken.value != 0) {
                self->RootGrid().ActualThemeChanged(self->m_actualThemeChangedToken);
                self->m_actualThemeChangedToken = {};
            }
            self->m_capturePlacementChanges = false;
            self->m_subclassInstalled = false;
        }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
} catch (...) {
    OutputDebugStringW(L"[AudioPlaybackConnector2] SettingsWindow subclass callback failed\n");
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handlers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::RootGrid_Loaded(IInspectable const&, RoutedEventArgs const&) {
    if (std::exchange(m_loaded, true)) return;

    try {
        LocalizeSettingsText();

        InitializeSettingsContent();
        const auto requestedPage = m_currentPage;
        ShowSettingsPage(SettingsPage::App);

        if (!m_hadPersistedPlacement) {
            ApplyAdaptiveLayout();
        }
        ShowSettingsPage(requestedPage);

        this->ExtendsContentIntoTitleBar(true);
        this->SetTitleBar(TitleBarArea());

        auto hwnd = util::GetWindowHandle(*this);
        if (!hwnd) winrt::throw_hresult(E_HANDLE);
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        ApplyCurrentWindowTheme(hwnd);
        if (m_actualThemeChangedToken.value == 0) {
            auto weak = get_weak();
            m_actualThemeChangedToken = RootGrid().ActualThemeChanged([weak](auto const&, auto const&) {
                if (auto self = weak.get()) {
                    self->ApplyCurrentWindowTheme(util::GetWindowHandle(*self));
                }
            });
        }

        auto appWindow = this->AppWindow();
        if (!appWindow) winrt::throw_hresult(E_HANDLE);
        appWindow.Resize({m_targetPlacement.size.cx, m_targetPlacement.size.cy});

        auto presenter = appWindow.Presenter().as<OverlappedPresenter>();
        if (presenter) {
            presenter.IsResizable(true);
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(false);
        }

        if (!m_subclassInstalled &&
            SetWindowSubclass(hwnd, SettingsWindowSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            m_subclassInstalled = true;
        }

        RevealAtTarget(hwnd);
        m_initializationState = InitializationState::Succeeded;
    } catch (winrt::hresult_error const& ex) {
        m_initializationState = InitializationState::Failed;
        util::DebugTraceException(L"[SettingsWindow] initialization failed", ex);
        CloseAfterInitializationFailure();
    } catch (std::exception const& ex) {
        m_initializationState = InitializationState::Failed;
        util::DebugTraceException(L"[SettingsWindow] initialization failed", ex);
        CloseAfterInitializationFailure();
    } catch (...) {
        m_initializationState = InitializationState::Failed;
        util::DebugTraceUnknownException(L"[SettingsWindow] initialization failed");
        CloseAfterInitializationFailure();
    }
}

SettingsWindow::InitializationState SettingsWindow::InitializationStatus() const noexcept {
    return m_initializationState;
}

void SettingsWindow::CloseAfterInitializationFailure() noexcept {
    try {
        Close();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] failed to close after initialization error", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] failed to close after initialization error", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] failed to close after initialization error");
    }
}

void SettingsWindow::LocalizeSettingsText() {
    this->Title(winrt::hstring(_("Settings_Title")));
    apc::ui::SetButtonLabel(SettingsHelpButton(), winrt::hstring(_("Settings_Help")));
    SettingsHelpText().Text(winrt::hstring(_("Settings_Help")));
    apc::ui::SetButtonLabel(SettingsBackButton(), winrt::hstring(_("DeviceOptions_Back")));
    DiagnosticsExpander().Header(box_value(winrt::hstring(_("Settings_Diagnostics"))));
    SettingsPageTitle().Text(
        winrt::hstring(m_currentPage == SettingsPage::App ? _("Settings_Title") : _("Settings_Help")));
    ConnectionGroupText().Text(winrt::hstring(_("Settings_Connections")));

    ConnectOnStartupLabel().Text(winrt::hstring(_("Settings_ConnectOnStartup")));
    apc::ui::SetTooltipText(ConnectOnStartupLabel(), winrt::hstring(_("Settings_ConnectOnStartup_Desc")));
    ReconnectOnConnectionLossLabel().Text(winrt::hstring(_("Settings_ReconnectOnConnectionLoss")));
    apc::ui::SetTooltipText(ReconnectOnConnectionLossLabel(),
                            winrt::hstring(_("Settings_ReconnectOnConnectionLoss_Desc")));
    AllowIncomingConnectionsLabel().Text(winrt::hstring(_("Settings_AllowIncomingConnections")));
    apc::ui::SetTooltipText(AllowIncomingConnectionsLabel(),
                            winrt::hstring(_("Settings_AllowIncomingConnections_Desc")));

    LanguageLabel().Text(winrt::hstring(_("Settings_Language")));
    SetItemContent(LanguageSystemItem(), _("Settings_System"));
    SetItemContent(LanguageEnglishItem(), _("Language_English"));
    SetItemContent(LanguageGermanItem(), _("Language_German"));
    SetItemContent(LanguageFrenchItem(), _("Language_French"));
    SetItemContent(LanguageSpanishItem(), _("Language_Spanish"));
    SetItemContent(LanguageJapaneseItem(), _("Language_Japanese"));
    SetItemContent(LanguageKoreanItem(), _("Language_Korean"));
    SetItemContent(LanguageChineseSimplifiedItem(), _("Language_ChineseSimplified"));
    SetItemContent(LanguageChineseTraditionalItem(), _("Language_ChineseTraditional"));
    StartWithWindowsLabel().Text(winrt::hstring(_("Settings_StartWithWindows")));
    apc::ui::SetTooltipText(StartWithWindowsLabel(), winrt::hstring(_("Settings_StartWithWindows_Desc")));
    ShowNotificationsLabel().Text(winrt::hstring(_("Settings_ShowNotifications")));
    apc::ui::SetTooltipText(ShowNotificationsLabel(), winrt::hstring(_("Settings_ShowNotifications_Desc")));
    SystemBackdropEffectsLabel().Text(winrt::hstring(_("Settings_SystemBackdropEffects")));
    apc::ui::SetTooltipText(SystemBackdropEffectsLabel(), winrt::hstring(_("Settings_SystemBackdropEffects_Desc")));
    WindowPlacementLabel().Text(winrt::hstring(_("Settings_WindowPlacement")));
    apc::ui::SetTooltipText(WindowPlacementLabel(), winrt::hstring(_("Settings_WindowPlacement_Desc")));
    apc::ui::SetButtonLabel(ResetWindowPlacementButton(), winrt::hstring(_("Settings_WindowPlacement_Reset")));

    PrivacyModeLabel().Text(winrt::hstring(_("Settings_PrivacyMode")));
    apc::ui::SetTooltipText(PrivacyModeLabel(), winrt::hstring(_("Settings_PrivacyMode_Desc")));
    xaml_automation::AutomationProperties::SetHelpText(PrivacyModeToggle(),
                                                       winrt::hstring(_("Settings_PrivacyMode_Desc")));

    DiagnosticsPrivacyText().Text(winrt::hstring(_("Settings_Diagnostics_PrivacyNote")));
    apc::ui::SetButtonLabel(
        TroubleshootingButton(), TroubleshootingButtonText(), winrt::hstring(_("Settings_Troubleshooting")));
    apc::ui::SetButtonLabel(
        CopyDiagnosticsButton(), CopyDiagnosticsButtonText(), winrt::hstring(_("Settings_CopyDiagnostics")));
    apc::ui::SetButtonLabel(ReportBugButton(), ReportBugButtonText(), winrt::hstring(_("Settings_ReportBug")));
    apc::ui::SetButtonLabel(
        FeatureRequestButton(), FeatureRequestButtonText(), winrt::hstring(_("Settings_FeatureRequest")));
    apc::ui::SetButtonLabel(OpenBluetoothSettingsButton(),
                            OpenBluetoothSettingsButtonText(),
                            winrt::hstring(_("Settings_OpenBluetoothSettings")));
    apc::ui::SetButtonLabel(
        OpenLogFolderButton(), OpenLogFolderButtonText(), winrt::hstring(_("Settings_OpenLogFolder")));

    VersionText().Text(winrt::hstring(BuildVersionText()));
    CopyrightText().Text(winrt::hstring(_("About_Copyright")));
    apc::ui::SetButtonLabel(RepositoryButton(), RepositoryButtonText(), winrt::hstring(_("Settings_Repository")));
    apc::ui::SetTooltipText(CheckForUpdatesButton(), winrt::hstring(_("Settings_CheckForUpdates_Desc")));
    apc::ui::SetButtonLabel(CheckForUpdatesButton(), winrt::hstring(_("Settings_CheckForUpdates")));
    apc::ui::SetButtonLabel(
        OpenAppInstallerButton(), OpenAppInstallerButtonText(), winrt::hstring(_("Settings_OpenAppInstaller")));

    SetAutomationName(ConnectOnStartupToggle(), _("Settings_ConnectOnStartup"));
    SetAutomationName(ReconnectOnConnectionLossToggle(), _("Settings_ReconnectOnConnectionLoss"));
    SetAutomationName(AllowIncomingConnectionsToggle(), _("Settings_AllowIncomingConnections"));
    SetAutomationName(PrivacyModeToggle(), _("Settings_PrivacyMode"));
    SetAutomationName(StartWithWindowsToggle(), _("Settings_StartWithWindows"));
    SetAutomationName(ShowNotificationsToggle(), _("Settings_ShowNotifications"));
    SetAutomationName(SystemBackdropEffectsToggle(), _("Settings_SystemBackdropEffects"));
    SetAutomationName(LanguageComboBox(), _("Settings_Language"));
}

void SettingsWindow::RevealAtTarget(HWND hwnd) {
    auto appWindow = this->AppWindow();
    if (appWindow) {
        appWindow.Move({m_targetPlacement.position.x, m_targetPlacement.position.y});
        appWindow.Show();
    }
    SetForegroundWindow(hwnd);
    m_capturePlacementChanges = true;
}

void SettingsWindow::QueuePlacementSave() {
    if (!m_capturePlacementChanges) return;
    try {
        if (!m_placementSaveTimer) {
            m_placementSaveTimer = DispatcherQueue().CreateTimer();
            m_placementSaveTimer.Interval(c_placementSaveDelay);
            m_placementSaveTimer.IsRepeating(false);
            auto weak = get_weak();
            m_placementSaveTimer.Tick([weak](auto const&, auto const&) noexcept {
                if (auto self = weak.get()) self->CommitPlacementNow();
            });
        }

        m_placementSaveTimer.Stop();
        m_placementSaveTimer.Start();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] QueuePlacementSave failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] QueuePlacementSave failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] QueuePlacementSave failed");
    }
}

void SettingsWindow::ApplyCurrentWindowTheme(HWND hwnd) noexcept {
    if (!hwnd) return;
    BOOL dark = RootGrid().ActualTheme() == ElementTheme::Dark;
    (void)DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void SettingsWindow::ApplySystemBackdropEffects(bool enabled) noexcept try {
    if (enabled) {
        SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop());
    } else {
        SystemBackdrop(nullptr);
    }
    RootGrid().Background(
        enabled
            ? winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent())
            : apc::ui::ThemeBrushOrFallback(L"SolidBackgroundFillColorBaseBrush", winrt::Windows::UI::Colors::White()));
} catch (...) {
    util::DebugTraceUnknownException(L"[SettingsWindow] failed to apply backdrop setting");
}

void SettingsWindow::CommitPlacementNow() noexcept {
    try {
        if (!StoreCurrentPlacement()) return;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CommitPlacementNow failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CommitPlacementNow failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] CommitPlacementNow failed");
    }
}

void SettingsWindow::StopPlacementSaveTimer() noexcept {
    try {
        if (m_placementSaveTimer) m_placementSaveTimer.Stop();
    } catch (...) {
    }
}

bool SettingsWindow::StoreCurrentPlacement() {
    if (!m_capturePlacementChanges) return false;

    auto controller = m_settingsController;
    if (!controller) return false;

    auto hwnd = util::GetWindowHandle(*this);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return false;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;

    auto width = rect.right - rect.left;
    auto height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;
    if (rect.left <= -30000 || rect.top <= -30000) return false;

    auto dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    return controller->SetSettingsWindowBounds(PersistedWindowBounds{static_cast<int32_t>(rect.left),
                                                                     static_cast<int32_t>(rect.top),
                                                                     static_cast<int32_t>(width),
                                                                     static_cast<int32_t>(height),
                                                                     dpi});
}

void SettingsWindow::StartWithWindowsToggle_Toggled(IInspectable const& sender, RoutedEventArgs const&) {
    if (m_suppressStartupToggle) return;
    auto toggle = sender.as<ToggleSwitch>();
    if (m_startupTaskCoordinator) m_startupTaskCoordinator->RequestDesired(toggle.IsOn());
}

void SettingsWindow::CheckForUpdatesButton_Click(IInspectable const&, RoutedEventArgs const&) {
    RunManualUpdateCheckAsync();
}

void SettingsWindow::OpenAppInstallerButton_Click(IInspectable const&, RoutedEventArgs const&) {
    UpdateService::LaunchAppInstallerAsync();
}

void SettingsWindow::ResetWindowPlacementButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ResetWindowPlacement();
}

void SettingsWindow::RepositoryButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(c_repositoryUrl);
}

void SettingsWindow::FeatureRequestButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(c_featureRequestUrl);
}

void SettingsWindow::TroubleshootingButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(c_troubleshootingUrl);
}

void SettingsWindow::OpenBluetoothSettingsButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(L"ms-settings:bluetooth");
}

void SettingsWindow::OpenLogFolderButton_Click(IInspectable const&, RoutedEventArgs const&) {
    OpenLogFolder();
}

void SettingsWindow::CopyDiagnosticsButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (m_diagnosticsCopyInProgress) return;
    try {
        auto controller = m_settingsController;
        auto snapshot = controller ? controller->Snapshot() : SettingsData{};
        auto connectedCount = controller ? controller->ConnectedDeviceCount() : 0;
        auto context = apc::ui::CaptureSettingsDiagnosticsReportContext(BuildVersionText());
        auto logPath = util::GetCachedLogPath();
        auto requestId = ++m_diagnosticsCopyRequestId;
        m_diagnosticsCopyInProgress = true;
        CopyDiagnosticsButton().IsEnabled(false);
        CopyDiagnosticsAsync(get_weak(),
                             winrt::apartment_context{},
                             std::move(snapshot),
                             connectedCount,
                             std::move(logPath),
                             std::move(context),
                             requestId);
        return;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] BuildDiagnosticsText failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] BuildDiagnosticsText failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] BuildDiagnosticsText failed");
    }

    m_diagnosticsCopyInProgress = false;
    try {
        CopyDiagnosticsButton().IsEnabled(true);
        ShowDiagnosticsInfo(
            InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
    } catch (...) {
    }
}

void SettingsWindow::ReportBugButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(BuildReportBugUri());
}

void SettingsWindow::SettingsHelpButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ShowSettingsPage(SettingsPage::Help);
    SettingsBackButton().Focus(FocusState::Programmatic);
}

void SettingsWindow::SettingsBackButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ShowSettingsPage(SettingsPage::App);
    SettingsHelpButton().Focus(FocusState::Programmatic);
}

void SettingsWindow::SettingsContentHost_SizeChanged(IInspectable const&, SizeChangedEventArgs const&) {
    UpdateContentPanelWidth();
}

void SettingsWindow::LanguageComboBox_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (m_suppressLanguageSelection) return;

    auto selected = LanguageComboBox().SelectedItem().try_as<ComboBoxItem>();
    if (!selected) return;

    auto language = winrt::unbox_value_or<winrt::hstring>(selected.Tag(), L"system");
    if (auto controller = m_settingsController) {
        controller->SetLanguage(std::wstring(language));
    }
    LocalizeSettingsText();
    ShowSettingsPage(m_currentPage);
    UpdateContentPanelWidth();
}

void SettingsWindow::ResetWindowPlacement() {
    StopPlacementSaveTimer();
    m_capturePlacementChanges = false;

    if (auto controller = m_settingsController) {
        static_cast<void>(controller->ClearSettingsWindowBounds());
    }

    ApplyAdaptiveLayout();
    auto appWindow = this->AppWindow();
    if (appWindow) {
        appWindow.Resize({m_targetPlacement.size.cx, m_targetPlacement.size.cy});
        appWindow.Move({m_targetPlacement.position.x, m_targetPlacement.position.y});
        appWindow.Show();
    }

    if (auto hwnd = util::GetWindowHandle(*this)) {
        SetForegroundWindow(hwnd);
    }

    m_capturePlacementChanges = true;
}

void SettingsWindow::ApplyAdaptiveLayout() {
    m_targetPlacement = CalculateAdaptivePlacement();
}

util::SettingsWindowPlacement SettingsWindow::CalculateAdaptivePlacement() {
    auto basePlacement = m_defaultPlacement;
    if (basePlacement.dpi == 0 || basePlacement.workArea.right <= basePlacement.workArea.left ||
        basePlacement.workArea.bottom <= basePlacement.workArea.top) {
        basePlacement = util::CalculateSettingsWindowPlacement();
    }

    auto const dpi = basePlacement.dpi == 0 ? USER_DEFAULT_SCREEN_DPI : basePlacement.dpi;
    auto const minTrackSize = util::GetSettingsWindowMinTrackSizeForWorkArea(basePlacement.workArea, dpi);
    auto const workWidth = std::max<int32_t>(1, basePlacement.workArea.right - basePlacement.workArea.left);
    auto const workHeight = std::max<int32_t>(1, basePlacement.workArea.bottom - basePlacement.workArea.top);
    auto const workWidthDip = PixelToDip(workWidth, dpi);
    auto const workHeightDip = PixelToDip(workHeight, dpi);
    auto const minWindowWidthDip = PixelToDip(minTrackSize.cx, dpi);
    auto const minWindowHeightDip = PixelToDip(minTrackSize.cy, dpi);
    auto const maxWindowWidthDip =
        std::max(minWindowWidthDip,
                 std::min(workWidthDip * 0.90, workWidthDip - static_cast<double>(c_settingsWindowEdgeMarginDip * 2)));
    auto const maxWindowHeightDip = std::max(
        minWindowHeightDip,
        std::min(workHeightDip * 0.90, workHeightDip - static_cast<double>(c_settingsWindowEdgeMarginDip * 2)));

    auto const padding = SettingsContentHost().Padding();
    auto const contentChromeWidth = padding.Left + padding.Right + c_settingsContentRightSafetyDip;
    auto const minContentWidth = std::max(320.0, minWindowWidthDip - contentChromeWidth);
    auto const maxContentWidth = std::max(minContentWidth, maxWindowWidthDip - contentChromeWidth);
    auto const lowerContentWidth = std::clamp(c_settingsContentMinWidthDip, minContentWidth, maxContentWidth);
    auto const upperContentWidth = std::max(lowerContentWidth, std::min(c_settingsContentMaxWidthDip, maxContentWidth));

    auto chosenContentWidth = lowerContentWidth;
    auto chosenContentHeight = MeasureVisibleContentHeight(chosenContentWidth);
    for (auto contentWidth = lowerContentWidth; contentWidth <= upperContentWidth + 0.1;
         contentWidth += c_settingsContentWidthStepDip) {
        auto const contentHeight = MeasureVisibleContentHeight(contentWidth);
        chosenContentWidth = contentWidth;
        chosenContentHeight = contentHeight;
        auto const windowHeight =
            c_settingsTitleBarHeightDip + c_settingsNavigationHeightDip + padding.Top + padding.Bottom + contentHeight;
        if (windowHeight <= maxWindowHeightDip) {
            break;
        }
    }

    auto const measuredWindowWidth = contentChromeWidth + chosenContentWidth;
    auto const measuredWindowHeight = c_settingsTitleBarHeightDip + c_settingsNavigationHeightDip + padding.Top +
                                      padding.Bottom + chosenContentHeight;
    auto const windowWidthDip = std::clamp(measuredWindowWidth, minWindowWidthDip, maxWindowWidthDip);
    auto const windowHeightDip = std::clamp(measuredWindowHeight, minWindowHeightDip, maxWindowHeightDip);
    auto const finalContentWidth = std::max(320.0, windowWidthDip - contentChromeWidth);
    SettingsContentPanel().Width(finalContentWidth);

    SIZE desiredSize{DipToPixelCeil(windowWidthDip, dpi), DipToPixelCeil(windowHeightDip, dpi)};
    return util::CalculateSettingsWindowPlacementFromSize(desiredSize, dpi, basePlacement);
}

double SettingsWindow::MeasureVisibleContentHeight(double contentWidth) {
    SettingsContentPanel().Width(contentWidth);
    SettingsContentPanel().Measure(
        {static_cast<float>(contentWidth), static_cast<float>(c_settingsMeasureInfinityDip)});
    return SettingsContentPanel().DesiredSize().Height;
}

void SettingsWindow::InitializeSettingsContent() {
    if (m_contentInitialized) return;
    m_contentInitialized = true;

    auto controller = m_settingsController;
    if (!controller) return;

    auto settings = m_initialSettingsSnapshot ? std::move(*m_initialSettingsSnapshot) : controller->Snapshot();
    m_initialSettingsSnapshot.reset();
    ConnectOnStartupToggle().IsOn(settings.GlobalConnectOnStartup);
    ReconnectOnConnectionLossToggle().IsOn(settings.GlobalReconnectOnConnectionLoss);
    AllowIncomingConnectionsToggle().IsOn(settings.AllowIncomingConnections);
    PrivacyModeToggle().IsOn(settings.PrivacyModeEnabled);
    SelectLanguage(settings.Language);
    ConnectOnStartupToggle().OffContent(box_value(L""));
    ConnectOnStartupToggle().OnContent(box_value(L""));
    ReconnectOnConnectionLossToggle().OffContent(box_value(L""));
    ReconnectOnConnectionLossToggle().OnContent(box_value(L""));
    AllowIncomingConnectionsToggle().OffContent(box_value(L""));
    AllowIncomingConnectionsToggle().OnContent(box_value(L""));
    PrivacyModeToggle().OffContent(box_value(L""));
    PrivacyModeToggle().OnContent(box_value(L""));
    auto weak = get_weak();
    ConnectOnStartupToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetGlobalConnectOnStartup(s.template as<ToggleSwitch>().IsOn());
            }
        }
    });

    ReconnectOnConnectionLossToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetGlobalReconnectOnConnectionLoss(s.template as<ToggleSwitch>().IsOn());
            }
        }
    });

    AllowIncomingConnectionsToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetAllowIncomingConnections(s.template as<ToggleSwitch>().IsOn());
            }
        }
    });

    PrivacyModeToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetPrivacyMode(s.template as<ToggleSwitch>().IsOn());
            }
        }
    });

    // Show cached value immediately; async init below corrects it from the actual task state.
    StartWithWindowsToggle().IsOn(settings.StartWithWindows);
    ShowNotificationsToggle().IsOn(settings.ShowNotifications);
    SystemBackdropEffectsToggle().IsOn(settings.UseSystemBackdropEffects);
    ApplySystemBackdropEffects(settings.UseSystemBackdropEffects);
    StartWithWindowsToggle().OffContent(box_value(L""));
    StartWithWindowsToggle().OnContent(box_value(L""));
    ShowNotificationsToggle().OffContent(box_value(L""));
    ShowNotificationsToggle().OnContent(box_value(L""));
    SystemBackdropEffectsToggle().OffContent(box_value(L""));
    SystemBackdropEffectsToggle().OnContent(box_value(L""));
    if (m_startupTaskCoordinator) {
        auto weakWindow = get_weak();
        auto dispatcher = DispatcherQueue();
        m_startupTaskHandlerToken =
            m_startupTaskCoordinator->Subscribe([weakWindow, dispatcher](StartupTaskSnapshot const& snapshot) noexcept {
                try {
                    auto apply = [weakWindow, snapshot]() noexcept {
                        if (auto self = weakWindow.get()) self->ApplyStartupTaskSnapshot(snapshot);
                    };
                    if (dispatcher.HasThreadAccess()) {
                        apply();
                    } else {
                        static_cast<void>(dispatcher.TryEnqueue(std::move(apply)));
                    }
                } catch (...) {
                }
            });
        m_startupTaskCoordinator->Refresh();
    } else {
        StartWithWindowsToggle().IsEnabled(false);
    }

    StartWithWindowsToggle().Toggled([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->StartWithWindowsToggle_Toggled(sender, args);
        }
    });

    ShowNotificationsToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetShowNotifications(s.template as<ToggleSwitch>().IsOn());
            }
        }
    });

    SystemBackdropEffectsToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            auto enabled = s.template as<ToggleSwitch>().IsOn();
            self->ApplySystemBackdropEffects(enabled);
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetSystemBackdropEffects(enabled);
            }
        }
    });
}

void SettingsWindow::ShowDiagnosticsInfo(InfoBarSeverity severity, std::wstring_view title, std::wstring_view message) {
    DiagnosticsInfoBar().Severity(severity);
    DiagnosticsInfoBar().Title(winrt::hstring(title));
    DiagnosticsInfoBar().Message(winrt::hstring(message));
    DiagnosticsInfoBar().IsOpen(true);
}

winrt::fire_and_forget SettingsWindow::LaunchUri(std::wstring_view uri) {
    auto weak = get_weak();
    auto uriCopy = std::wstring(uri);
    winrt::apartment_context uiThread;
    bool launched = false;
    try {
        launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(uriCopy));
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] LaunchUri failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] LaunchUri failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] LaunchUri failed");
    }

    try {
        co_await uiThread;
        auto self = weak.get();
        if (self && !launched) {
            self->ShowDiagnosticsInfo(
                InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
        }
    } catch (...) {
    }
}

void SettingsWindow::OpenLogFolder() {
    try {
        auto folder = util::GetCachedLogPath().parent_path();
        if (folder.empty()) {
            ShowDiagnosticsInfo(
                InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        auto result = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            ShowDiagnosticsInfo(
                InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] OpenLogFolder failed", ex);
        ShowDiagnosticsInfo(
            InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] OpenLogFolder failed", ex);
        ShowDiagnosticsInfo(
            InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] OpenLogFolder failed");
        ShowDiagnosticsInfo(
            InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
    }
}

bool SettingsWindow::CopyTextToClipboard(std::wstring_view text) {
    try {
        auto package = winrt::Windows::ApplicationModel::DataTransfer::DataPackage();
        package.SetText(winrt::hstring(text));
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CopyTextToClipboard failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CopyTextToClipboard failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] CopyTextToClipboard failed");
    }

    return false;
}

void SettingsWindow::UpdateContentPanelWidth() {
    auto const hostWidth = SettingsContentHost().ActualWidth();
    auto const padding = SettingsContentHost().Padding();
    auto const availableWidth = hostWidth - padding.Left - padding.Right - c_settingsContentRightSafetyDip;
    if (availableWidth <= 0) return;

    SettingsContentPanel().Width(std::min(availableWidth, c_settingsContentMaxWidthDip));
}

void SettingsWindow::SelectLanguage(std::wstring_view language) {
    auto target = language.empty() ? std::wstring_view(L"system") : language;
    m_suppressLanguageSelection = true;

    auto selectIfTagMatches = [&](ComboBoxItem const& item) {
        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        if (tag == target) {
            LanguageComboBox().SelectedItem(item);
            return true;
        }
        return false;
    };

    if (selectIfTagMatches(LanguageSystemItem()) || selectIfTagMatches(LanguageEnglishItem()) ||
        selectIfTagMatches(LanguageGermanItem()) || selectIfTagMatches(LanguageFrenchItem()) ||
        selectIfTagMatches(LanguageSpanishItem()) || selectIfTagMatches(LanguageJapaneseItem()) ||
        selectIfTagMatches(LanguageKoreanItem()) || selectIfTagMatches(LanguageChineseSimplifiedItem()) ||
        selectIfTagMatches(LanguageChineseTraditionalItem())) {
        m_suppressLanguageSelection = false;
        return;
    }

    LanguageComboBox().SelectedItem(LanguageSystemItem());
    m_suppressLanguageSelection = false;
}

std::wstring SettingsWindow::BuildReportBugUri() const {
    auto body = std::wstring(_("Settings_ReportBug_BodyPrefix"));
    return std::wstring(c_bugReportUrl) + L"&title=" + PercentEncode(_("Settings_ReportBug_DefaultTitle")) + L"&body=" +
           PercentEncode(body);
}

winrt::fire_and_forget SettingsWindow::RunManualUpdateCheckAsync() {
    auto weak = get_weak();
    auto updateCoordinator = m_updateCoordinator;
    auto requestId = ++m_updateCheckRequestId;
    SetUpdateCheckBusy(true);
    bool completed = false;
    winrt::apartment_context ui;

    try {
        UpdateCheckResult result;
        if (updateCoordinator) {
            result = co_await updateCoordinator->CheckForUpdatesAsync(UpdateCheckReason::Manual);
        } else {
            result = co_await UpdateService::CheckForUpdatesAsync();
        }
        co_await ui;

        auto self = weak.get();
        if (!self || requestId != self->m_updateCheckRequestId.load()) co_return;
        self->SetUpdateCheckBusy(false);
        if (result.Status == UpdateCheckStatus::Cancelled) co_return;
        self->ShowUpdateCheckResult(result);
        completed = true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] RunManualUpdateCheckAsync failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] RunManualUpdateCheckAsync failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] RunManualUpdateCheckAsync failed");
    }

    try {
        co_await ui;
        auto self = weak.get();
        if (self && !completed && requestId == self->m_updateCheckRequestId.load()) {
            self->SetUpdateCheckBusy(false);
            self->ShowUpdateCheckResult(UpdateCheckResult{UpdateCheckStatus::Failed, L"", L""});
        }
    } catch (...) {
    }
}

void SettingsWindow::SetUpdateCheckBusy(bool busy) {
    CheckForUpdatesButton().IsEnabled(!busy);
    UpdateCheckProgress().IsActive(busy);
    UpdateCheckProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    if (busy) {
        OpenAppInstallerButton().Visibility(Visibility::Collapsed);
        UpdateInfoBar().IsOpen(false);
    }
}

void SettingsWindow::SetStartupTaskBusy(bool busy) {
    StartWithWindowsToggle().IsEnabled(!busy);
    StartupTaskProgress().IsActive(busy);
    StartupTaskProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
}

void SettingsWindow::ShowUpdateCheckResult(UpdateCheckResult const& result) {
    OpenAppInstallerButton().Visibility(Visibility::Collapsed);

    switch (result.Status) {
        case UpdateCheckStatus::UpdateAvailable:
            UpdateInfoBar().Severity(InfoBarSeverity::Informational);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateAvailable_Title")));
            UpdateInfoBar().Message(
                winrt::hstring(util::ReplacePlaceholders(_("Settings_UpdateAvailable_Message"), result.LatestVersion)));
            OpenAppInstallerButton().Visibility(Visibility::Visible);
            break;
        case UpdateCheckStatus::UpToDate:
            UpdateInfoBar().Severity(InfoBarSeverity::Success);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateCurrent_Title")));
            UpdateInfoBar().Message(winrt::hstring(_("Settings_UpdateCurrent_Message")));
            break;
        case UpdateCheckStatus::Cancelled: return;
        case UpdateCheckStatus::Failed:
        default:
            UpdateInfoBar().Severity(InfoBarSeverity::Error);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateFailed_Title")));
            if (!result.ErrorMessage.empty()) {
                DebugTrace(L"[SettingsWindow] Manual update check failed: {0}", result.ErrorMessage);
            }
            UpdateInfoBar().Message(winrt::hstring(_("Settings_UpdateFailed_Message")));
            break;
    }

    UpdateInfoBar().IsOpen(true);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Startup Integration ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::ApplyStartupTaskSnapshot(StartupTaskSnapshot const& snapshot) noexcept try {
    if (snapshot.Publication < m_lastStartupTaskPublication) return;
    m_lastStartupTaskPublication = snapshot.Publication;
    SetStartupTaskBusy(snapshot.Busy);
    if (!snapshot.Known) return;

    m_suppressStartupToggle = true;
    auto restoreSuppression = wil::scope_exit([this]() noexcept { m_suppressStartupToggle = false; });
    StartWithWindowsToggle().IsOn(snapshot.Enabled);
} catch (...) {
    m_suppressStartupToggle = false;
    util::DebugTraceUnknownException(L"[SettingsWindow] startup task snapshot ignored exception");
}

winrt::fire_and_forget SettingsWindow::CopyDiagnosticsAsync(winrt::weak_ref<SettingsWindow> weak,
                                                            winrt::apartment_context uiThread,
                                                            SettingsData settings,
                                                            std::size_t connectedDeviceCount,
                                                            std::filesystem::path logPath,
                                                            apc::ui::SettingsDiagnosticsReportContext context,
                                                            std::uint64_t requestId) {
    apc::ui::DiagnosticsLogResult logResult;
    std::wstring diagnostics;
    bool returnedToUi = false;
    try {
        co_await winrt::resume_background();
        logResult =
            apc::ui::CollectRecentDiagnosticLogLines(logPath, settings, context.RedactedDevice, context.RedactedValue);
        diagnostics = apc::ui::BuildSettingsDiagnosticsReport(settings, connectedDeviceCount, context, logResult);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CollectRecentDiagnosticLogLines failed", ex);
        logResult.Status = apc::ui::DiagnosticsLogStatus::Unavailable;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CollectRecentDiagnosticLogLines failed", ex);
        logResult.Status = apc::ui::DiagnosticsLogStatus::Unavailable;
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] CollectRecentDiagnosticLogLines failed");
        logResult.Status = apc::ui::DiagnosticsLogStatus::Unavailable;
    }

    try {
        co_await uiThread;
        returnedToUi = true;
        auto self = weak.get();
        if (!self || requestId != self->m_diagnosticsCopyRequestId) co_return;
        if (diagnostics.empty() || !self->CopyTextToClipboard(diagnostics)) {
            self->ShowDiagnosticsInfo(
                InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
            self->m_diagnosticsCopyInProgress = false;
            self->CopyDiagnosticsButton().IsEnabled(true);
            co_return;
        }
        self->ShowDiagnosticsInfo(
            InfoBarSeverity::Success, _("Settings_DiagnosticsCopied_Title"), _("Settings_DiagnosticsCopied_Message"));
        self->m_diagnosticsCopyInProgress = false;
        self->CopyDiagnosticsButton().IsEnabled(true);
        co_return;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CopyDiagnosticsAsync failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] CopyDiagnosticsAsync failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] CopyDiagnosticsAsync failed");
    }

    if (returnedToUi) {
        try {
            auto self = weak.get();
            if (self && requestId == self->m_diagnosticsCopyRequestId) {
                self->m_diagnosticsCopyInProgress = false;
                self->CopyDiagnosticsButton().IsEnabled(true);
                self->ShowDiagnosticsInfo(
                    InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
            }
        } catch (...) {
        }
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::SetSettingsController(std::shared_ptr<ISettingsController> controller) {
    m_settingsController = std::move(controller);
}

void SettingsWindow::SetStartupTaskCoordinator(std::shared_ptr<StartupTaskCoordinator> coordinator) {
    m_startupTaskCoordinator = std::move(coordinator);
}

void SettingsWindow::SetInitialSettingsSnapshot(SettingsData snapshot) {
    m_hadPersistedPlacement = snapshot.SettingsWindowBounds.has_value();
    m_initialSettingsSnapshot = std::move(snapshot);
}

void SettingsWindow::SetUpdateCoordinator(std::shared_ptr<UpdateCoordinator> coordinator) {
    m_updateCoordinator = std::move(coordinator);
}

void SettingsWindow::SetDefaultPlacement(util::SettingsWindowPlacement placement) {
    m_defaultPlacement = placement;
}

void SettingsWindow::SetTargetPlacement(util::SettingsWindowPlacement placement) {
    m_targetPlacement = placement;
}

} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::implementation {
void SettingsWindow::ShowSettingsPage(SettingsPage page) {
    const bool changed = m_currentPage != page;
    StopPageTransition();
    m_currentPage = page;
    AppSection().Visibility(page == SettingsPage::App ? Visibility::Visible : Visibility::Collapsed);
    SettingsBackButton().Visibility(page == SettingsPage::Help ? Visibility::Visible : Visibility::Collapsed);
    SettingsHelpButton().Visibility(page == SettingsPage::App ? Visibility::Visible : Visibility::Collapsed);
    SettingsPageTitle().Text(winrt::hstring(page == SettingsPage::App ? _("Settings_Title") : _("Settings_Help")));
    HelpSection().Visibility(page == SettingsPage::Help ? Visibility::Visible : Visibility::Collapsed);
    SettingsContentHost().ChangeView(nullptr, 0.0, nullptr);
    UpdateContentPanelWidth();
    if (changed && InitializationStatus() == InitializationState::Succeeded) {
        try {
            if (!winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) return;
            using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
            DoubleAnimation fade;
            fade.From(0.2);
            fade.To(1.0);
            fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(150)));
            fade.FillBehavior(FillBehavior::Stop);
            Storyboard::SetTarget(fade,
                                  page == SettingsPage::App ? AppSection().as<FrameworkElement>()
                                                            : HelpSection().as<FrameworkElement>());
            Storyboard::SetTargetProperty(fade, L"Opacity");
            m_pageTransition = Storyboard();
            m_pageTransition.Children().Append(fade);
            m_pageTransition.Begin();
        } catch (...) {
            util::DebugTraceUnknownException(L"[SettingsWindow] Page transition failed");
            StopPageTransition();
        }
    }
}
void SettingsWindow::StopPageTransition() noexcept {
    try {
        auto animation = std::exchange(m_pageTransition, nullptr);
        if (animation) animation.Stop();
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] Failed to stop page transition");
    }
}
void SettingsWindow::ShowHelpPage() {
    if (!m_loaded) {
        m_currentPage = SettingsPage::Help;
        return;
    }
    ShowSettingsPage(SettingsPage::Help);
    SettingsBackButton().Focus(FocusState::Programmatic);
}
} // namespace winrt::AudioPlaybackConnector2::implementation
