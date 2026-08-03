#include <pch.h>
#include <ui/SettingsWindow/SettingsWindow.xaml.h>
#if __has_include("SettingsWindow.g.cpp")
#include <SettingsWindow.g.cpp>
#endif

#include <core/Settings.hpp>
#include <core/StringResources.hpp>
#include <core/ThemeHelper.hpp>
#include <services/StartupTaskController.hpp>
#include <services/UpdateCoordinator.hpp>
#include <services/UpdateService.hpp>
#include <ui/ButtonHelpers.hpp>
#include <util/Logger.hpp>
#include <util/Util.hpp>
#include <ui/XamlWindowInterop.hpp>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Text.h>

#include <cmath>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Windowing;
namespace xaml_automation = winrt::Microsoft::UI::Xaml::Automation;

namespace {
constexpr auto c_placementSaveDelay = std::chrono::seconds(3);
constexpr auto c_aliasSavedFeedbackDelay = std::chrono::seconds(2);
constexpr std::wstring_view c_repositoryUrl = L"https://github.com/N0ahTM/AudioPlaybackConnector2";
constexpr std::wstring_view c_bugReportUrl = L"https://github.com/N0ahTM/AudioPlaybackConnector2/issues/new?labels=bug";
constexpr std::wstring_view c_featureRequestUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/issues/new?labels=enhancement";
constexpr std::wstring_view c_troubleshootingUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/blob/main/docs/TROUBLESHOOTING.md";
constexpr std::wstring_view c_streamDeckSetupUrl =
    L"https://github.com/N0ahTM/AudioPlaybackConnector2/tree/main/integrations/stream-deck";
constexpr double c_navigationPaneMinLengthDip = 116.0;
constexpr double c_navigationPaneChromeDip = 90.0;
constexpr double c_settingsContentMinWidthDip = 480.0;
constexpr double c_settingsContentMaxWidthDip = 680.0;
constexpr double c_settingsContentWidthStepDip = 20.0;
constexpr double c_settingsTitleBarHeightDip = 32.0;
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

double MeasureTextWidth(std::wstring_view text, double fontSize) {
    auto probe = TextBlock();
    probe.Text(winrt::hstring(text));
    probe.FontSize(fontSize);
    probe.TextWrapping(TextWrapping::NoWrap);
    probe.Measure({static_cast<float>(c_settingsMeasureInfinityDip), static_cast<float>(c_settingsMeasureInfinityDip)});
    return probe.DesiredSize().Width;
}

std::wstring NavigationItemText(NavigationViewItem const& item) {
    auto content = item.Content();
    if (!content) return {};

    if (auto textBlock = content.try_as<TextBlock>()) {
        return std::wstring(textBlock.Text());
    }
    return std::wstring(winrt::unbox_value_or<winrt::hstring>(content, L""));
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

std::wstring RedactedDeviceDisplay(DeviceSettings const& device) {
    (void)device;
    return std::wstring(_("Privacy_RedactedDevice"));
}

void SetAutomationName(DependencyObject const& element, std::wstring_view name) {
    xaml_automation::AutomationProperties::SetName(element, winrt::hstring(name));
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
    if (!localAppData.empty()) {
        ReplaceAll(value, localAppData, L"%LOCALAPPDATA%");
    }

    auto userProfile = EnvironmentValue(L"USERPROFILE");
    if (!userProfile.empty()) {
        ReplaceAll(value, userProfile, L"%USERPROFILE%");
    }

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

std::wstring RedactDiagnosticLine(std::wstring line, SettingsData const& snapshot) {
    line = RedactKnownPaths(std::move(line));
    for (auto const& device : snapshot.Devices) {
        ReplaceAll(line, device.Id, _("Privacy_RedactedValue"));
        if (!device.Name.empty()) {
            ReplaceAll(line, device.Name, RedactedDeviceDisplay(device));
        }
        if (!device.Alias.empty()) {
            ReplaceAll(line, device.Alias, RedactedDeviceDisplay(device));
        }
    }
    return RedactSensitivePatterns(std::move(line));
}

std::wstring DefaultDeviceModeText(DefaultDeviceMode mode) {
    return mode == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
}

std::wstring ToLowerInvariant(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return result;
}

std::wstring TrimWhitespace(std::wstring value) {
    auto first = std::ranges::find_if_not(value, [](wchar_t ch) { return std::iswspace(ch) != 0; });
    auto last = std::ranges::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
                    return std::iswspace(ch) != 0;
                }).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

void SetItemContent(ComboBoxItem const& item, std::wstring_view text) {
    item.Content(winrt::box_value(winrt::hstring(text)));
}

void SetItemContent(NavigationViewItem const& item, std::wstring_view text) {
    item.Content(winrt::box_value(winrt::hstring(text)));
}

std::vector<std::wstring>
RecentRelevantLogLines(std::filesystem::path const& path, std::size_t maxLines, SettingsData const& snapshot) {
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
        matches.push_back(RedactDiagnosticLine(std::move(line), snapshot));
        if (matches.size() > maxLines) {
            matches.erase(matches.begin());
        }
    }
    return matches;
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
    StopPlacementSaveTimer();
    if (m_themeChangedToken != 0) {
        ThemeHelper::RemoveThemeChangedHandler(m_themeChangedToken);
    }
}

LRESULT CALLBACK SettingsWindow::SettingsWindowSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto self = reinterpret_cast<SettingsWindow*>(dwRefData);
    if (msg == WM_GETMINMAXINFO) {
        auto minSize = util::GetSettingsWindowMinTrackSize(hwnd);
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = minSize.cx;
        info->ptMinTrackSize.y = minSize.cy;
        return 0;
    }

    if (self && msg == WM_CLOSE) {
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
            if (self->m_themeChangedToken != 0) {
                ThemeHelper::RemoveThemeChangedHandler(self->m_themeChangedToken);
                self->m_themeChangedToken = 0;
            }
            self->m_capturePlacementChanges = false;
            self->m_subclassInstalled = false;
        }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handlers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::RootGrid_Loaded(IInspectable const&, RoutedEventArgs const&) {
    if (std::exchange(m_loaded, true)) return;

    LocalizeSettingsText();

    SetAutomationName(ConnectOnStartupToggle(), _("Settings_ConnectOnStartup"));
    SetAutomationName(ReconnectOnConnectionLossToggle(), _("Settings_ReconnectOnConnectionLoss"));
    SetAutomationName(AllowIncomingConnectionsToggle(), _("Settings_AllowIncomingConnections"));
    SetAutomationName(PrivacyModeToggle(), _("Settings_PrivacyMode"));
    SetAutomationName(StartWithWindowsToggle(), _("Settings_StartWithWindows"));
    SetAutomationName(ShowNotificationsToggle(), _("Settings_ShowNotifications"));
    SetAutomationName(LanguageComboBox(), _("Settings_Language"));

    InitializeSettingsContent();
    m_suppressNavigationSelection = true;
    SettingsNavigation().SelectedItem(DevicesNavItem());
    m_suppressNavigationSelection = false;
    ShowSettingsPage(SettingsPage::Devices);

    bool restoredPlacement = false;
    if (auto controller = m_settingsController) {
        restoredPlacement = controller->Snapshot().SettingsWindowBounds.has_value();
    }
    if (!restoredPlacement) {
        ApplyAdaptiveLayout();
    } else {
        SettingsNavigation().OpenPaneLength(CalculateNavigationPaneLength());
    }
    m_adaptiveLayoutReady = true;

    this->ExtendsContentIntoTitleBar(true);
    this->SetTitleBar(TitleBarArea());

    auto hwnd = util::GetWindowHandle(*this);
    if (hwnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        ApplyCurrentWindowTheme(hwnd);
        if (m_themeChangedToken == 0) {
            auto weak = get_weak();
            m_themeChangedToken = ThemeHelper::AddThemeChangedHandler([weak]() {
                if (auto self = weak.get()) {
                    self->ApplyCurrentWindowTheme(util::GetWindowHandle(*self));
                }
            });
        }

        auto appWindow = this->AppWindow();
        if (appWindow) {
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
        }
    }
}

void SettingsWindow::LocalizeSettingsText() {
    this->Title(winrt::hstring(_("Settings_Title")));
    SetItemContent(DevicesNavItem(), _("Settings_Devices"));
    SetItemContent(AppNavItem(), _("Settings_App"));
    SetItemContent(PrivacyNavItem(), _("Settings_Privacy"));
    SetItemContent(StreamDeckNavItem(), _("Settings_StreamDeck"));
    StreamDeckBetaIcon().Glyph(winrt::hstring(_("Settings_BetaBadge")));
    SetItemContent(HelpNavItem(), _("Settings_Help"));
    SetItemContent(InfoNavItem(), _("Settings_About"));

    DefaultDeviceLabel().Text(winrt::hstring(_("Settings_DefaultDevice")));
    DefaultDeviceDesc().Text(winrt::hstring(_("Settings_DefaultDevice_Desc")));
    apc::ui::SetButtonLabel(DefaultLastConnectedButton(),
                            DefaultLastConnectedButtonText(),
                            winrt::hstring(_("Settings_DefaultDevice_LastConnected")));
    apc::ui::SetButtonLabel(
        ClearDefaultDeviceButton(), ClearDefaultDeviceButtonText(), winrt::hstring(_("Settings_DefaultDevice_Clear")));
    ConnectOnStartupLabel().Text(winrt::hstring(_("Settings_ConnectOnStartup")));
    ConnectOnStartupDesc().Text(winrt::hstring(_("Settings_ConnectOnStartup_Desc")));
    ReconnectOnConnectionLossLabel().Text(winrt::hstring(_("Settings_ReconnectOnConnectionLoss")));
    ReconnectOnConnectionLossDesc().Text(winrt::hstring(_("Settings_ReconnectOnConnectionLoss_Desc")));
    AllowIncomingConnectionsLabel().Text(winrt::hstring(_("Settings_AllowIncomingConnections")));
    AllowIncomingConnectionsDesc().Text(winrt::hstring(_("Settings_AllowIncomingConnections_Desc")));

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
    StartWithWindowsDesc().Text(winrt::hstring(_("Settings_StartWithWindows_Desc")));
    ShowNotificationsLabel().Text(winrt::hstring(_("Settings_ShowNotifications")));
    ShowNotificationsDesc().Text(winrt::hstring(_("Settings_ShowNotifications_Desc")));
    WindowPlacementLabel().Text(winrt::hstring(_("Settings_WindowPlacement")));
    WindowPlacementDesc().Text(winrt::hstring(_("Settings_WindowPlacement_Desc")));
    apc::ui::SetButtonLabel(ResetWindowPlacementButton(),
                            ResetWindowPlacementButtonText(),
                            winrt::hstring(_("Settings_WindowPlacement_Reset")));

    PrivacyModeLabel().Text(winrt::hstring(_("Settings_PrivacyMode")));
    PrivacyModeDesc().Text(winrt::hstring(_("Settings_PrivacyMode_Desc")));

    StreamDeckDesc().Text(winrt::hstring(_("Settings_StreamDeck_Desc")));
    apc::ui::SetButtonLabel(
        StreamDeckSetupButton(), StreamDeckSetupButtonText(), winrt::hstring(_("Settings_StreamDeck_SetupGuide")));

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
    CheckForUpdatesLabel().Text(winrt::hstring(_("Settings_CheckForUpdates")));
    CheckForUpdatesDesc().Text(winrt::hstring(_("Settings_CheckForUpdates_Desc")));
    apc::ui::SetButtonLabel(
        CheckForUpdatesButton(), CheckForUpdatesButtonText(), winrt::hstring(_("Settings_CheckForUpdates_Button")));
    apc::ui::SetButtonLabel(
        OpenAppInstallerButton(), OpenAppInstallerButtonText(), winrt::hstring(_("Settings_OpenAppInstaller")));

    SetAutomationName(ConnectOnStartupToggle(), _("Settings_ConnectOnStartup"));
    SetAutomationName(ReconnectOnConnectionLossToggle(), _("Settings_ReconnectOnConnectionLoss"));
    SetAutomationName(AllowIncomingConnectionsToggle(), _("Settings_AllowIncomingConnections"));
    SetAutomationName(PrivacyModeToggle(), _("Settings_PrivacyMode"));
    SetAutomationName(StartWithWindowsToggle(), _("Settings_StartWithWindows"));
    SetAutomationName(ShowNotificationsToggle(), _("Settings_ShowNotifications"));
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
                if (auto self = weak.get()) self->SavePlacementNow();
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
    BOOL dark = ThemeHelper::GetSystemTheme() == Theme::Dark;
    (void)DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void SettingsWindow::SavePlacementNow() noexcept {
    try {
        if (!StoreCurrentPlacement()) return;
        if (auto controller = m_settingsController) controller->Save();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] SavePlacementNow failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] SavePlacementNow failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] SavePlacementNow failed");
    }
}

void SettingsWindow::StopPlacementSaveTimer() noexcept {
    try {
        if (m_placementSaveTimer) m_placementSaveTimer.Stop();
    } catch (...) {
    }
}

winrt::fire_and_forget SettingsWindow::ClearAliasSavedAfterDelayAsync(uint64_t requestId) {
    auto lifetime = get_strong();
    try {
        winrt::apartment_context uiThread;
        co_await winrt::resume_after(c_aliasSavedFeedbackDelay);
        co_await uiThread;

        if (requestId != m_aliasSavedRequestId.load()) co_return;
        m_aliasSavedDeviceId.clear();
        if (m_currentPage == SettingsPage::Devices) {
            RebuildDeviceList();
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] ClearAliasSavedAfterDelayAsync failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] ClearAliasSavedAfterDelayAsync failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] ClearAliasSavedAfterDelayAsync failed");
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
    ApplyStartWithWindowsAsync(toggle.IsOn());
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

void SettingsWindow::DefaultLastConnectedButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto controller = m_settingsController) {
        controller->SetDefaultDeviceMode(DefaultDeviceMode::LastConnected);
    }
    RebuildDeviceList();
}

void SettingsWindow::ClearDefaultDeviceButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto controller = m_settingsController) {
        controller->ClearDefaultDevice();
    }
    RebuildDeviceList();
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
    std::wstring diagnostics;
    try {
        diagnostics = BuildDiagnosticsText();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindow] BuildDiagnosticsText failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindow] BuildDiagnosticsText failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindow] BuildDiagnosticsText failed");
    }

    if (diagnostics.empty() || !CopyTextToClipboard(diagnostics)) {
        ShowDiagnosticsInfo(
            InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
        return;
    }

    ShowDiagnosticsInfo(
        InfoBarSeverity::Success, _("Settings_DiagnosticsCopied_Title"), _("Settings_DiagnosticsCopied_Message"));
}

void SettingsWindow::ReportBugButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(BuildReportBugUri());
}

void SettingsWindow::StreamDeckSetupButton_Click(IInspectable const&, RoutedEventArgs const&) {
    LaunchUri(c_streamDeckSetupUrl);
}

void SettingsWindow::SettingsNavigation_SelectionChanged(IInspectable const&,
                                                         NavigationViewSelectionChangedEventArgs const& args) {
    if (m_suppressNavigationSelection) return;

    auto selected = args.SelectedItem().try_as<NavigationViewItem>();
    if (!selected) return;

    auto tag = winrt::unbox_value_or<winrt::hstring>(selected.Tag(), L"");
    if (tag == L"devices") {
        ShowSettingsPage(SettingsPage::Devices);
    } else if (tag == L"app") {
        ShowSettingsPage(SettingsPage::App);
    } else if (tag == L"privacy") {
        ShowSettingsPage(SettingsPage::Privacy);
    } else if (tag == L"streamDeck") {
        ShowSettingsPage(SettingsPage::StreamDeck);
    } else if (tag == L"help") {
        ShowSettingsPage(SettingsPage::Help);
    } else if (tag == L"about") {
        ShowSettingsPage(SettingsPage::About);
    }

    if (m_adaptiveLayoutReady) ApplyAdaptiveLayout();
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
    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), std::wstring(language));
    LocalizeSettingsText();
    ShowSettingsPage(m_currentPage);
    RebuildDeviceList();
    ApplyAdaptiveLayout();
}

void SettingsWindow::ResetWindowPlacement() {
    StopPlacementSaveTimer();
    m_capturePlacementChanges = false;

    if (auto controller = m_settingsController) {
        if (controller->ClearSettingsWindowBounds()) {
            controller->Save();
        }
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

    auto const navigationWidth = CalculateNavigationPaneLength();
    SettingsNavigation().OpenPaneLength(navigationWidth);

    auto const padding = SettingsContentHost().Padding();
    auto const contentChromeWidth = navigationWidth + padding.Left + padding.Right + c_settingsContentRightSafetyDip;
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
        auto const windowHeight = c_settingsTitleBarHeightDip + padding.Top + padding.Bottom + contentHeight;
        if (windowHeight <= maxWindowHeightDip) {
            break;
        }
    }

    auto const measuredWindowWidth = contentChromeWidth + chosenContentWidth;
    auto const measuredWindowHeight = c_settingsTitleBarHeightDip + padding.Top + padding.Bottom + chosenContentHeight;
    auto const windowWidthDip = std::clamp(measuredWindowWidth, minWindowWidthDip, maxWindowWidthDip);
    auto const windowHeightDip = std::clamp(measuredWindowHeight, minWindowHeightDip, maxWindowHeightDip);
    auto const finalContentWidth = std::max(320.0, windowWidthDip - contentChromeWidth);
    SettingsContentPanel().Width(finalContentWidth);

    SIZE desiredSize{DipToPixelCeil(windowWidthDip, dpi), DipToPixelCeil(windowHeightDip, dpi)};
    return util::CalculateSettingsWindowPlacementFromSize(desiredSize, dpi, basePlacement);
}

double SettingsWindow::CalculateNavigationPaneLength() {
    std::array<NavigationViewItem, 6> items{
        DevicesNavItem(), AppNavItem(), PrivacyNavItem(), StreamDeckNavItem(), HelpNavItem(), InfoNavItem()};

    auto maxLabelWidth = 0.0;
    for (auto const& item : items) {
        maxLabelWidth = std::max(maxLabelWidth, MeasureTextWidth(NavigationItemText(item), 14.0));
    }
    auto const streamDeckLabelWidth = MeasureTextWidth(_("Settings_StreamDeck"), 14.0) + 24.0;
    maxLabelWidth = std::max(maxLabelWidth, streamDeckLabelWidth);

    return std::max(c_navigationPaneMinLengthDip, std::ceil(maxLabelWidth + c_navigationPaneChromeDip));
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

    {
        auto settings = controller->Snapshot();
        ConnectOnStartupToggle().IsOn(settings.GlobalConnectOnStartup);
        ReconnectOnConnectionLossToggle().IsOn(settings.GlobalReconnectOnConnectionLoss);
        AllowIncomingConnectionsToggle().IsOn(settings.AllowIncomingConnections);
        PrivacyModeToggle().IsOn(settings.PrivacyModeEnabled);
        SelectLanguage(settings.Language);
    }
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
            self->RebuildDeviceList();
        }
    });

    ReconnectOnConnectionLossToggle().Toggled([weak](auto const& s, auto) {
        if (auto self = weak.get()) {
            if (auto settingsController = self->m_settingsController) {
                settingsController->SetGlobalReconnectOnConnectionLoss(s.template as<ToggleSwitch>().IsOn());
            }
            self->RebuildDeviceList();
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
            self->RebuildDeviceList();
        }
    });

    // Show cached value immediately; async init below corrects it from the actual task state.
    {
        auto settings = controller->Snapshot();
        StartWithWindowsToggle().IsOn(settings.StartWithWindows);
        ShowNotificationsToggle().IsOn(settings.ShowNotifications);
    }
    StartWithWindowsToggle().OffContent(box_value(L""));
    StartWithWindowsToggle().OnContent(box_value(L""));
    ShowNotificationsToggle().OffContent(box_value(L""));
    ShowNotificationsToggle().OnContent(box_value(L""));
    SyncStartupTaskStateAsync();

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

    RebuildDeviceList();
}

void SettingsWindow::ShowDiagnosticsInfo(InfoBarSeverity severity, std::wstring_view title, std::wstring_view message) {
    DiagnosticsInfoBar().Severity(severity);
    DiagnosticsInfoBar().Title(winrt::hstring(title));
    DiagnosticsInfoBar().Message(winrt::hstring(message));
    DiagnosticsInfoBar().IsOpen(true);
}

winrt::fire_and_forget SettingsWindow::LaunchUri(std::wstring_view uri) {
    auto lifetime = get_strong();
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
        if (!launched) {
            ShowDiagnosticsInfo(
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

void SettingsWindow::ShowSettingsPage(SettingsPage page) {
    m_currentPage = page;

    DevicesSection().Visibility(page == SettingsPage::Devices ? Visibility::Visible : Visibility::Collapsed);
    AppSection().Visibility(page == SettingsPage::App ? Visibility::Visible : Visibility::Collapsed);
    PrivacySection().Visibility(page == SettingsPage::Privacy ? Visibility::Visible : Visibility::Collapsed);
    StreamDeckSection().Visibility(page == SettingsPage::StreamDeck ? Visibility::Visible : Visibility::Collapsed);
    HelpSection().Visibility(page == SettingsPage::Help ? Visibility::Visible : Visibility::Collapsed);
    InfoSection().Visibility(page == SettingsPage::About ? Visibility::Visible : Visibility::Collapsed);

    switch (page) {
        case SettingsPage::Devices:
            TitleText().Text(winrt::hstring(_("Settings_Devices")));
            SubtitleText().Text(winrt::hstring(_("Settings_Devices_Intro")));
            break;
        case SettingsPage::App:
            TitleText().Text(winrt::hstring(_("Settings_App")));
            SubtitleText().Text(winrt::hstring(_("Settings_App_Intro")));
            break;
        case SettingsPage::Privacy:
            TitleText().Text(winrt::hstring(_("Settings_Privacy")));
            SubtitleText().Text(winrt::hstring(_("Settings_Privacy_Intro")));
            break;
        case SettingsPage::StreamDeck:
            TitleText().Text(winrt::hstring(_("Settings_StreamDeck")));
            SubtitleText().Text(winrt::hstring(_("Settings_StreamDeck_Intro")));
            break;
        case SettingsPage::Help:
            TitleText().Text(winrt::hstring(_("Settings_Help")));
            SubtitleText().Text(winrt::hstring(_("Settings_Help_Intro")));
            break;
        case SettingsPage::About:
            TitleText().Text(winrt::hstring(_("Settings_About")));
            SubtitleText().Text(winrt::hstring(_("Settings_About_Intro")));
            break;
    }

    TitleText().StartBringIntoView();
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

void SettingsWindow::CommitAlias(std::wstring const& deviceId,
                                 TextBox const& textBox,
                                 std::wstring const& previousAlias,
                                 bool alwaysRebuild) {
    auto alias = TrimWhitespace(std::wstring(textBox.Text()));
    textBox.Text(winrt::hstring(alias));

    if (alias == previousAlias) {
        if (alwaysRebuild) {
            RebuildDeviceList();
        }
        return;
    }

    if (auto settingsController = m_settingsController) {
        if (!settingsController->SetDeviceAlias(deviceId, alias)) {
            ShowDiagnosticsInfo(
                InfoBarSeverity::Error, _("Settings_ActionFailed_Title"), _("Settings_ActionFailed_Message"));
            return;
        }
    }
    m_aliasSavedDeviceId = deviceId;
    auto requestId = ++m_aliasSavedRequestId;
    RebuildDeviceList();
    ClearAliasSavedAfterDelayAsync(requestId);
}

std::wstring SettingsWindow::BuildDiagnosticsText() const {
    std::wstringstream output;
    auto controller = m_settingsController;
    auto snapshot = controller ? controller->Snapshot() : SettingsData{};
    auto connectedCount = controller ? controller->ConnectedDeviceCount() : 0;
    auto logPath = util::GetCachedLogPath();

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
    output << _("Settings_Diagnostics_AppVersion") << L": " << BuildVersionText() << L"\n";
    output << _("Settings_Diagnostics_WindowsVersion") << L": " << windowsVersion << L"\n";
    output << _("Settings_Diagnostics_PackageIdentity") << L": " << packageIdentity << L"\n";
    output << _("Settings_Diagnostics_InstallType") << L": " << installType << L"\n";
    output << _("Settings_Diagnostics_PrivacyMode") << L": "
           << (snapshot.PrivacyModeEnabled ? _("Settings_On") : _("Settings_Off")) << L"\n";
    output << _("Settings_Diagnostics_ConnectedCount") << L": " << connectedCount << L"\n";
    output << _("Settings_Diagnostics_KnownDevices") << L": " << snapshot.Devices.size() << L"\n";
    output << _("Settings_Diagnostics_DefaultMode") << L": " << DefaultDeviceModeText(snapshot.DefaultDevice) << L"\n";
    output << _("Settings_Diagnostics_LogPath") << L": " << RedactKnownPaths(logPath.wstring()) << L"\n";
    output << L"\n" << _("Settings_Diagnostics_Devices") << L":\n";

    if (snapshot.Devices.empty()) {
        output << L"- " << _("Settings_NoDevices") << L"\n";
    } else {
        for (auto const& device : snapshot.Devices) {
            output << L"- " << RedactedDeviceDisplay(device);
            if (snapshot.DefaultDevice == DefaultDeviceMode::SpecificDevice && snapshot.DefaultDeviceId == device.Id) {
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

    auto recentErrors = RecentRelevantLogLines(logPath, 5, snapshot);
    output << L"\n" << _("Settings_Diagnostics_RecentErrors") << L":\n";
    if (recentErrors.empty()) {
        output << L"- " << _("Settings_Diagnostics_NoRecentErrors") << L"\n";
    } else {
        for (auto const& line : recentErrors) {
            output << L"- " << line << L"\n";
        }
    }

    output << L"\n" << _("Settings_Diagnostics_PrivacyNote") << L"\n";
    output << _("Settings_Diagnostics_DumpWarning") << L"\n";
    return output.str();
}

std::wstring SettingsWindow::BuildReportBugUri() const {
    auto body = std::wstring(_("Settings_ReportBug_BodyPrefix"));
    return std::wstring(c_bugReportUrl) + L"&title=" + PercentEncode(_("Settings_ReportBug_DefaultTitle")) + L"&body=" +
           PercentEncode(body);
}

winrt::fire_and_forget SettingsWindow::RunManualUpdateCheckAsync() {
    auto lifetime = get_strong();
    auto requestId = ++m_updateCheckRequestId;
    SetUpdateCheckBusy(true);
    bool completed = false;
    winrt::apartment_context ui;

    try {
        UpdateCheckResult result;
        if (m_updateCoordinator) {
            result = co_await m_updateCoordinator->CheckForUpdatesAsync(UpdateCheckReason::Manual);
        } else {
            result = co_await UpdateService::CheckForUpdatesAsync();
        }
        co_await ui;

        if (requestId != m_updateCheckRequestId.load()) co_return;
        SetUpdateCheckBusy(false);
        if (result.Status == UpdateCheckStatus::Cancelled) co_return;
        ShowUpdateCheckResult(result);
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
        if (!completed && requestId == m_updateCheckRequestId.load()) {
            SetUpdateCheckBusy(false);
            ShowUpdateCheckResult(UpdateCheckResult{UpdateCheckStatus::Failed, L"", L""});
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

winrt::fire_and_forget SettingsWindow::SyncStartupTaskStateAsync() {
    auto lifetime = get_strong();
    winrt::apartment_context ui;
    auto requestId = m_startupRequestId.load();
    SetStartupTaskBusy(true);

    try {
        bool enabled = co_await StartupTaskController::IsEnabledAsync();

        co_await ui;
        if (requestId != m_startupRequestId.load()) co_return;
        SetStartupTaskBusy(false);

        m_suppressStartupToggle = true;
        StartWithWindowsToggle().IsOn(enabled);
        m_suppressStartupToggle = false;
        if (auto settingsController = m_settingsController) {
            settingsController->SetStartWithWindows(enabled);
        }
        co_return;
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[SettingsWindow] SyncStartupTaskStateAsync failed: 0x{0:08X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
    } catch (std::exception const& ex) {
        DebugTrace(L"[SettingsWindow] SyncStartupTaskStateAsync failed: {0}", util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[SettingsWindow] SyncStartupTaskStateAsync failed: unknown exception");
    }

    try {
        co_await ui;
        if (requestId == m_startupRequestId.load()) {
            SetStartupTaskBusy(false);
        }
    } catch (...) {
    }
}

winrt::fire_and_forget SettingsWindow::ApplyStartWithWindowsAsync(bool on) {
    auto lifetime = get_strong();
    winrt::apartment_context ui;
    auto requestId = ++m_startupRequestId;
    bool revertToggle = false;
    SetStartupTaskBusy(true);

    try {
        bool success = co_await StartupTaskController::SetEnabledAsync(on);

        co_await ui;
        if (requestId != m_startupRequestId.load()) co_return;
        SetStartupTaskBusy(false);

        if (!success) {
            if (auto settingsController = m_settingsController) {
                settingsController->SetStartWithWindows(!on);
            }
            m_suppressStartupToggle = true;
            StartWithWindowsToggle().IsOn(!on);
            m_suppressStartupToggle = false;
            co_return;
        }

        if (auto settingsController = m_settingsController) {
            settingsController->SetStartWithWindows(on);
        }
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[SettingsWindow] ApplyStartWithWindowsAsync failed: 0x{0:08X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
        revertToggle = true;
    } catch (std::exception const& ex) {
        DebugTrace(L"[SettingsWindow] ApplyStartWithWindowsAsync failed: {0}", util::Utf8ToUtf16(ex.what()));
        revertToggle = true;
    } catch (...) {
        DebugTrace(L"[SettingsWindow] ApplyStartWithWindowsAsync failed: unknown exception");
        revertToggle = true;
    }

    if (revertToggle) {
        try {
            co_await ui;
            if (requestId != m_startupRequestId.load()) co_return;
            SetStartupTaskBusy(false);
            if (auto settingsController = m_settingsController) {
                settingsController->SetStartWithWindows(!on);
            }
            m_suppressStartupToggle = true;
            StartWithWindowsToggle().IsOn(!on);
            m_suppressStartupToggle = false;
        } catch (...) {
        }
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::RebuildDeviceList() {
    DevicesPanel().Children().Clear();

    auto controller = m_settingsController;
    if (!controller) return;

    auto secondaryBrush =
        apc::ui::ThemeBrushOrFallback(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Colors::Gray());

    // Snapshot settings through the controller, then build UI without holding any settings lock.
    auto snapshot = controller->Snapshot();
    auto devices = SettingsViewModel::BuildDeviceItems(snapshot);
    bool globalConnectOnStartup = snapshot.GlobalConnectOnStartup;
    bool globalReconnectOnConnectionLoss = snapshot.GlobalReconnectOnConnectionLoss;
    ClearDefaultDeviceButton().IsEnabled(snapshot.DefaultDevice == DefaultDeviceMode::SpecificDevice);
    DefaultLastConnectedButton().IsEnabled(snapshot.DefaultDevice != DefaultDeviceMode::LastConnected);

    if (devices.empty()) {
        auto emptyPanel = StackPanel();
        emptyPanel.Spacing(8);

        auto noDevices = TextBlock();
        noDevices.Text(winrt::hstring(_("Settings_NoDevices")));
        noDevices.Foreground(secondaryBrush);
        noDevices.FontSize(12);
        noDevices.TextWrapping(TextWrapping::Wrap);
        emptyPanel.Children().Append(noDevices);

        auto noDevicesDesc = TextBlock();
        noDevicesDesc.Text(winrt::hstring(_("Settings_NoDevices_Desc")));
        noDevicesDesc.Foreground(secondaryBrush);
        noDevicesDesc.FontSize(12);
        noDevicesDesc.TextWrapping(TextWrapping::Wrap);
        emptyPanel.Children().Append(noDevicesDesc);

        auto emptyActions = StackPanel();
        emptyActions.Orientation(Orientation::Vertical);
        emptyActions.HorizontalAlignment(HorizontalAlignment::Stretch);
        emptyActions.Spacing(8);

        auto bluetoothContent = StackPanel();
        bluetoothContent.Orientation(Orientation::Horizontal);
        bluetoothContent.Spacing(6);
        auto icon = FontIcon();
        icon.FontSize(14);
        icon.Glyph(L"\xE702");
        bluetoothContent.Children().Append(icon);
        auto label = TextBlock();
        label.Text(winrt::hstring(_("Settings_OpenBluetoothSettings")));
        label.TextWrapping(TextWrapping::WrapWholeWords);
        label.MaxWidth(420);
        bluetoothContent.Children().Append(label);

        auto bluetoothButton = Button();
        bluetoothButton.HorizontalAlignment(HorizontalAlignment::Stretch);
        bluetoothButton.Content(bluetoothContent);
        auto weak = get_weak();
        bluetoothButton.Click([weak](auto, auto) {
            if (auto self = weak.get()) {
                self->LaunchUri(L"ms-settings:bluetooth");
            }
        });
        emptyActions.Children().Append(bluetoothButton);

        auto troubleshootingContent = StackPanel();
        troubleshootingContent.Orientation(Orientation::Horizontal);
        troubleshootingContent.Spacing(6);
        auto troubleshootingIcon = FontIcon();
        troubleshootingIcon.FontSize(14);
        troubleshootingIcon.Glyph(L"\xE946");
        troubleshootingContent.Children().Append(troubleshootingIcon);
        auto troubleshootingLabel = TextBlock();
        troubleshootingLabel.Text(winrt::hstring(_("Settings_Troubleshooting")));
        troubleshootingLabel.TextWrapping(TextWrapping::WrapWholeWords);
        troubleshootingLabel.MaxWidth(420);
        troubleshootingContent.Children().Append(troubleshootingLabel);

        auto troubleshootingButton = Button();
        troubleshootingButton.HorizontalAlignment(HorizontalAlignment::Stretch);
        troubleshootingButton.Content(troubleshootingContent);
        troubleshootingButton.Click([weak](auto, auto) {
            if (auto self = weak.get()) {
                self->LaunchUri(c_troubleshootingUrl);
            }
        });
        emptyActions.Children().Append(troubleshootingButton);
        emptyPanel.Children().Append(emptyActions);
        DevicesPanel().Children().Append(emptyPanel);
        return;
    }

    for (auto& dev : devices) {
        auto item = Grid();
        item.Margin({0, 8, 0, 8});
        item.HorizontalAlignment(HorizontalAlignment::Stretch);
        item.RowSpacing(8);
        auto nameRow = RowDefinition();
        nameRow.Height(GridLengthHelper::Auto());
        auto aliasRow = RowDefinition();
        aliasRow.Height(GridLengthHelper::Auto());
        auto policyRow = RowDefinition();
        policyRow.Height(GridLengthHelper::Auto());
        auto actionsRow = RowDefinition();
        actionsRow.Height(GridLengthHelper::Auto());
        item.RowDefinitions().Append(nameRow);
        item.RowDefinitions().Append(aliasRow);
        item.RowDefinitions().Append(policyRow);
        item.RowDefinitions().Append(actionsRow);

        auto namePanel = StackPanel();
        namePanel.MinWidth(0);
        namePanel.VerticalAlignment(VerticalAlignment::Center);
        auto name = TextBlock();
        name.Text(dev.DisplayName);
        name.TextWrapping(TextWrapping::Wrap);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        name.MaxLines(2);
        apc::ui::SetTooltipText(name, winrt::hstring(dev.DisplayName));
        namePanel.Children().Append(name);

        auto subtitle = TextBlock();
        if (dev.IsDefaultDevice) {
            subtitle.Text(winrt::hstring(_("Settings_DefaultDevice_Current")));
        } else {
            subtitle.Text(winrt::hstring(_("Settings_PairedDevice")));
        }
        subtitle.Foreground(secondaryBrush);
        subtitle.FontSize(12);
        subtitle.TextWrapping(TextWrapping::Wrap);
        namePanel.Children().Append(subtitle);

        if (m_aliasSavedDeviceId == dev.Id) {
            auto savedStatus = TextBlock();
            savedStatus.Text(winrt::hstring(_("Settings_AliasSaved")));
            savedStatus.Foreground(
                apc::ui::ThemeBrushOrFallback(L"AccentTextFillColorPrimaryBrush", winrt::Windows::UI::Colors::Green()));
            savedStatus.FontSize(12);
            savedStatus.TextWrapping(TextWrapping::Wrap);
            namePanel.Children().Append(savedStatus);
        }

        Grid::SetRow(namePanel, 0);

        auto actionPanel = StackPanel();
        actionPanel.Orientation(Orientation::Horizontal);
        actionPanel.VerticalAlignment(VerticalAlignment::Center);
        actionPanel.Spacing(8);
        Grid::SetRow(actionPanel, 3);

        auto aliasBox = TextBox();
        aliasBox.HorizontalAlignment(HorizontalAlignment::Stretch);
        aliasBox.MinWidth(0);
        aliasBox.PlaceholderText(winrt::hstring(_("Settings_DeviceAliasPlaceholder")));
        aliasBox.Text(winrt::hstring(dev.Alias));
        apc::ui::SetTooltipText(aliasBox, winrt::hstring(_("Settings_DeviceAlias")));
        auto weak = get_weak();
        aliasBox.LostFocus([id = dev.Id, previousAlias = dev.Alias, weak](auto const& s, auto) {
            if (auto self = weak.get()) {
                auto textBox = s.template as<TextBox>();
                self->CommitAlias(id, textBox, previousAlias, false);
            }
        });
        aliasBox.KeyDown([id = dev.Id, previousAlias = dev.Alias, weak](auto const& s, auto const& e) {
            auto key = e.Key();
            auto textBox = s.template as<TextBox>();
            if (key == winrt::Windows::System::VirtualKey::Enter) {
                if (auto self = weak.get()) {
                    self->CommitAlias(id, textBox, previousAlias, true);
                }
                e.Handled(true);
            } else if (key == winrt::Windows::System::VirtualKey::Escape) {
                textBox.Text(winrt::hstring(previousAlias));
                if (auto self = weak.get()) {
                    self->RebuildDeviceList();
                }
                e.Handled(true);
            }
        });
        Grid::SetRow(aliasBox, 1);

        apc::ui::IconButtonOptions defaultOptions;
        defaultOptions.Width = 36;
        defaultOptions.Height = 32;
        defaultOptions.IconFontSize = 14;
        defaultOptions.TransparentBackground = false;
        defaultOptions.Borderless = false;
        auto defaultText =
            winrt::hstring(dev.IsDefaultDevice ? _("Settings_DefaultDevice_Current") : _("Settings_DefaultDevice_Set"));
        auto defaultBtn =
            apc::ui::CreateIconButton(dev.IsDefaultDevice ? L"\xE73E" : L"\xE8AB", defaultText, defaultOptions);
        defaultBtn.IsEnabled(!dev.IsDefaultDevice);
        defaultBtn.Click([id = dev.Id, weak](auto, auto) {
            if (auto self = weak.get()) {
                if (auto settingsController = self->m_settingsController) {
                    settingsController->SetDefaultDeviceId(id);
                }
                self->RebuildDeviceList();
            }
        });

        apc::ui::IconButtonOptions clearAliasOptions;
        clearAliasOptions.Width = 36;
        clearAliasOptions.Height = 32;
        clearAliasOptions.IconFontSize = 14;
        clearAliasOptions.TransparentBackground = false;
        clearAliasOptions.Borderless = false;
        auto clearAliasBtn =
            apc::ui::CreateIconButton(L"\xE894", winrt::hstring(_("Settings_DeviceAlias_Clear")), clearAliasOptions);
        clearAliasBtn.IsEnabled(!dev.Alias.empty());
        clearAliasBtn.Click([id = dev.Id, weak](auto, auto) {
            if (auto self = weak.get()) {
                if (auto settingsController = self->m_settingsController) {
                    settingsController->SetDeviceAlias(id, L"");
                }
                self->m_aliasSavedDeviceId = id;
                auto requestId = ++self->m_aliasSavedRequestId;
                self->RebuildDeviceList();
                self->ClearAliasSavedAfterDelayAsync(requestId);
            }
        });

        auto policyPanel = StackPanel();
        policyPanel.Spacing(6);
        Grid::SetRow(policyPanel, 2);

        auto startupPolicy = Grid();
        startupPolicy.ColumnSpacing(12);
        auto startupLabelColumn = ColumnDefinition();
        startupLabelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        auto startupToggleColumn = ColumnDefinition();
        startupToggleColumn.Width(GridLengthHelper::Auto());
        startupPolicy.ColumnDefinitions().Append(startupLabelColumn);
        startupPolicy.ColumnDefinitions().Append(startupToggleColumn);
        auto startupLabel = TextBlock();
        startupLabel.Text(winrt::hstring(_("Device_ConnectOnStartup")));
        startupLabel.FontSize(12);
        startupLabel.TextWrapping(TextWrapping::Wrap);
        auto startupToggle = ToggleSwitch();
        startupToggle.IsOn(globalConnectOnStartup || dev.ConnectOnStartup);
        startupToggle.IsEnabled(!globalConnectOnStartup);
        startupToggle.MinWidth(64);
        startupToggle.OffContent(box_value(L""));
        startupToggle.OnContent(box_value(L""));
        SetAutomationName(startupToggle, _("Device_ConnectOnStartup"));
        Grid::SetColumn(startupToggle, 1);
        startupToggle.Toggled([id = dev.Id, weak](auto const& s, auto) {
            if (auto self = weak.get()) {
                if (auto settingsController = self->m_settingsController) {
                    settingsController->SetDeviceConnectOnStartup(id, s.template as<ToggleSwitch>().IsOn());
                }
            }
        });
        startupPolicy.Children().Append(startupLabel);
        startupPolicy.Children().Append(startupToggle);
        policyPanel.Children().Append(startupPolicy);

        auto reconnectPolicy = Grid();
        reconnectPolicy.ColumnSpacing(12);
        auto reconnectLabelColumn = ColumnDefinition();
        reconnectLabelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        auto reconnectToggleColumn = ColumnDefinition();
        reconnectToggleColumn.Width(GridLengthHelper::Auto());
        reconnectPolicy.ColumnDefinitions().Append(reconnectLabelColumn);
        reconnectPolicy.ColumnDefinitions().Append(reconnectToggleColumn);
        auto reconnectLabel = TextBlock();
        reconnectLabel.Text(winrt::hstring(_("Device_ReconnectOnConnectionLoss")));
        reconnectLabel.FontSize(12);
        reconnectLabel.TextWrapping(TextWrapping::Wrap);
        auto reconnectToggle = ToggleSwitch();
        reconnectToggle.IsOn(globalReconnectOnConnectionLoss || dev.ReconnectOnConnectionLoss);
        reconnectToggle.IsEnabled(!globalReconnectOnConnectionLoss);
        reconnectToggle.MinWidth(64);
        reconnectToggle.OffContent(box_value(L""));
        reconnectToggle.OnContent(box_value(L""));
        SetAutomationName(reconnectToggle, _("Device_ReconnectOnConnectionLoss"));
        Grid::SetColumn(reconnectToggle, 1);
        reconnectToggle.Toggled([id = dev.Id, weak](auto const& s, auto) {
            if (auto self = weak.get()) {
                if (auto settingsController = self->m_settingsController) {
                    settingsController->SetDeviceReconnectOnConnectionLoss(id, s.template as<ToggleSwitch>().IsOn());
                }
            }
        });
        reconnectPolicy.Children().Append(reconnectLabel);
        reconnectPolicy.Children().Append(reconnectToggle);
        policyPanel.Children().Append(reconnectPolicy);

        apc::ui::IconButtonOptions forgetOptions;
        forgetOptions.Width = 36;
        forgetOptions.Height = 32;
        forgetOptions.IconFontSize = 14;
        forgetOptions.Foreground = apc::ui::TryThemeBrush(L"SystemFillColorCriticalBrush");
        forgetOptions.TransparentBackground = false;
        forgetOptions.Borderless = false;
        auto forgetText = winrt::hstring(_("Device_Forget"));
        auto forgetBtn = apc::ui::CreateIconButton(L"\xE74D", forgetText, forgetOptions);
        forgetBtn.Click([id = dev.Id, weak](auto, auto) {
            if (auto self = weak.get()) {
                if (auto settingsController = self->m_settingsController) {
                    settingsController->ForgetDevice(id);
                }
                self->RebuildDeviceList();
            }
        });

        item.Children().Append(namePanel);
        item.Children().Append(aliasBox);
        item.Children().Append(policyPanel);
        actionPanel.Children().Append(defaultBtn);
        actionPanel.Children().Append(clearAliasBtn);
        actionPanel.Children().Append(forgetBtn);
        item.Children().Append(actionPanel);
        DevicesPanel().Children().Append(item);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::SetSettingsController(std::shared_ptr<ISettingsController> controller) {
    m_settingsController = std::move(controller);
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
