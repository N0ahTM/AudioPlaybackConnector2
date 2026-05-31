#include <pch.h>
#include <ui/SettingsWindow/SettingsWindow.xaml.h>
#if __has_include("SettingsWindow.g.cpp")
#include <SettingsWindow.g.cpp>
#endif

#include <winrt/Windows.ApplicationModel.h>

#include <App/App.xaml.h>
#include <core/Settings.hpp>
#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <core/ThemeHelper.hpp>
#include <services/UpdateService.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Windowing;

namespace {
std::wstring BuildVersionText() {
    std::wstring label(_("About_Version"));
    try {
        auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        auto versionText =
            version.Revision == 0
                ? std::format(L"{}.{}.{}", version.Major, version.Minor, version.Build)
                : std::format(L"{}.{}.{}.{}", version.Major, version.Minor, version.Build, version.Revision);
        return std::format(L"{} {}", label, versionText);
    } catch (...) {
        return label;
    }
}

std::wstring ReplacePlaceholders(std::wstring_view templateStr, std::wstring_view replacement) {
    std::wstring result;
    size_t pos = 0;
    while (pos < templateStr.size()) {
        auto found = templateStr.find(L"{0}", pos);
        if (found == std::wstring_view::npos) {
            result.append(templateStr.substr(pos));
            break;
        }
        result.append(templateStr.substr(pos, found - pos));
        result.append(replacement);
        pos = found + 3;
    }
    return result;
}
} // namespace

namespace winrt::AudioPlaybackConnector2::implementation {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsWindow::SettingsWindow() {}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handlers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::RootGrid_Loaded(IInspectable const&, RoutedEventArgs const&) {
    // Localize static text before the window becomes visible.
    this->Title(winrt::hstring(_("Settings_Title")));
    TitleText().Text(winrt::hstring(_("Settings_Title")));
    SubtitleText().Text(winrt::hstring(_("Settings_Subtitle")));
    ConnectionHeader().Text(winrt::hstring(_("Settings_Connection")));
    AutoReconnectLabel().Text(winrt::hstring(_("Settings_AutoReconnect")));
    AutoReconnectDesc().Text(winrt::hstring(_("Settings_AutoReconnect_Desc")));
    SystemHeader().Text(winrt::hstring(_("Settings_System")));
    StartWithWindowsLabel().Text(winrt::hstring(_("Settings_StartWithWindows")));
    StartWithWindowsDesc().Text(winrt::hstring(_("Settings_StartWithWindows_Desc")));
    AppHeader().Text(winrt::hstring(_("Settings_App")));
    VersionText().Text(winrt::hstring(BuildVersionText()));
    CopyrightText().Text(winrt::hstring(_("About_Copyright")));
    UpdatesHeader().Text(winrt::hstring(_("Settings_Updates")));
    CheckForUpdatesLabel().Text(winrt::hstring(_("Settings_CheckForUpdates")));
    CheckForUpdatesDesc().Text(winrt::hstring(_("Settings_CheckForUpdates_Desc")));
    CheckForUpdatesButton().Content(box_value(winrt::hstring(_("Settings_CheckForUpdates_Button"))));
    OpenAppInstallerButton().Content(box_value(winrt::hstring(_("Settings_OpenAppInstaller"))));

    auto app = App::GetInstance();
    if (!app) return;

    {
        auto locked = app->GetSettings().LockSharedData();
        AutoReconnectToggle().IsOn(locked->GlobalAutoReconnect);
    }
    AutoReconnectToggle().OffContent(box_value(L""));
    AutoReconnectToggle().OnContent(box_value(L""));
    AutoReconnectToggle().Toggled([](auto const& s, auto) {
        auto app = App::GetInstance();
        if (!app) return;
        bool enabled = s.template as<ToggleSwitch>().IsOn();
        std::vector<DeviceSettings> devices;
        {
            auto locked = app->GetSettings().LockExclusiveData();
            locked->GlobalAutoReconnect = enabled;
            devices = locked->Devices;
        }
        if (auto manager = app->GetDeviceManager()) {
            for (const auto& c : manager->GetConnectedDevices()) {
                bool autoReconnect = enabled;
                for (const auto& d : devices) {
                    if (d.Id == c.Device.Id()) {
                        autoReconnect = autoReconnect || d.AutoReconnect;
                        break;
                    }
                }
                manager->SetAutoReconnect(c.Device.Id(), autoReconnect);
            }
        }
        app->GetSettings().Save(GetModuleHandleW(nullptr));
    });

    // Show cached value immediately; async init below corrects it from the actual task state.
    {
        auto locked = app->GetSettings().LockSharedData();
        StartWithWindowsToggle().IsOn(locked->StartWithWindows);
    }
    StartWithWindowsToggle().OffContent(box_value(L""));
    StartWithWindowsToggle().OnContent(box_value(L""));
    SyncStartupTaskStateAsync();

    auto weak = get_weak();
    StartWithWindowsToggle().Toggled([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->StartWithWindowsToggle_Toggled(sender, args);
        }
    });

    RebuildDeviceList();

    // Mica system backdrop for the settings window.
    this->SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop());

    // Extend content into title bar for seamless Mica look (Windows 11 style).
    this->ExtendsContentIntoTitleBar(true);
    this->SetTitleBar(TitleBarArea());

    auto hwnd = util::GetWindowHandle(*this);
    if (hwnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        BOOL dark = ThemeHelper::GetSystemTheme() == Theme::Dark;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        // Only show the window once content and DWM styling are fully ready.
        auto appWindow = this->AppWindow();
        if (appWindow) {
            RECT rcWork{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
            int x, y;
            if (m_targetX != INT_MIN && m_targetY != INT_MIN) {
                x = m_targetX;
                y = m_targetY;
            } else {
                x = rcWork.left + ((rcWork.right - rcWork.left) - c_settingsWindowWidth) / 2;
                y = rcWork.top + ((rcWork.bottom - rcWork.top) - c_settingsWindowHeight) / 2;
            }
            appWindow.Move({x, y});
            appWindow.Resize({c_settingsWindowWidth, c_settingsWindowHeight});

            auto presenter = appWindow.Presenter().as<OverlappedPresenter>();
            if (presenter) {
                presenter.IsResizable(false);
                presenter.IsMinimizable(false);
                presenter.IsMaximizable(false);
            }

            appWindow.Show();
        }
        SetForegroundWindow(hwnd);
    }
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

winrt::fire_and_forget SettingsWindow::RunManualUpdateCheckAsync() {
    auto lifetime = get_strong();
    auto requestId = ++m_updateCheckRequestId;
    SetUpdateCheckBusy(true);

    winrt::apartment_context ui;
    co_await winrt::resume_background();
    auto result = UpdateService::CheckForUpdates();
    co_await ui;

    if (requestId != m_updateCheckRequestId.load()) co_return;
    SetUpdateCheckBusy(false);
    ShowUpdateCheckResult(result);
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

void SettingsWindow::ShowUpdateCheckResult(UpdateCheckResult const& result) {
    OpenAppInstallerButton().Visibility(Visibility::Collapsed);

    switch (result.Status) {
        case UpdateCheckStatus::UpdateAvailable:
            UpdateInfoBar().Severity(InfoBarSeverity::Informational);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateAvailable_Title")));
            UpdateInfoBar().Message(
                winrt::hstring(ReplacePlaceholders(_("Settings_UpdateAvailable_Message"), result.LatestVersion)));
            OpenAppInstallerButton().Visibility(Visibility::Visible);
            break;
        case UpdateCheckStatus::UpToDate:
            UpdateInfoBar().Severity(InfoBarSeverity::Success);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateCurrent_Title")));
            UpdateInfoBar().Message(winrt::hstring(_("Settings_UpdateCurrent_Message")));
            break;
        case UpdateCheckStatus::Failed:
        default:
            UpdateInfoBar().Severity(InfoBarSeverity::Error);
            UpdateInfoBar().Title(winrt::hstring(_("Settings_UpdateFailed_Title")));
            UpdateInfoBar().Message(result.ErrorMessage.empty() ? winrt::hstring(_("Settings_UpdateFailed_Message"))
                                                                : winrt::hstring(result.ErrorMessage));
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

    try {
        auto task =
            co_await winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"AudioPlaybackConnector2StartupTask");
        bool enabled = (task.State() == winrt::Windows::ApplicationModel::StartupTaskState::Enabled ||
                        task.State() == winrt::Windows::ApplicationModel::StartupTaskState::EnabledByPolicy);

        co_await ui;
        if (requestId != m_startupRequestId.load()) co_return;

        m_suppressStartupToggle = true;
        StartWithWindowsToggle().IsOn(enabled);
        m_suppressStartupToggle = false;
        if (auto app = App::GetInstance()) {
            {
                auto locked = app->GetSettings().LockExclusiveData();
                locked->StartWithWindows = enabled;
            }
            app->GetSettings().Save(GetModuleHandleW(nullptr));
        }
    } catch (...) {
    }
}

winrt::fire_and_forget SettingsWindow::ApplyStartWithWindowsAsync(bool on) {
    auto lifetime = get_strong();
    winrt::apartment_context ui;
    auto requestId = ++m_startupRequestId;
    bool revertToggle = false;

    try {
        auto task =
            co_await winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"AudioPlaybackConnector2StartupTask");
        bool currentlyEnabled = (task.State() == winrt::Windows::ApplicationModel::StartupTaskState::Enabled ||
                                 task.State() == winrt::Windows::ApplicationModel::StartupTaskState::EnabledByPolicy);
        if (on == currentlyEnabled) {
            co_await ui;
            if (requestId != m_startupRequestId.load()) co_return;
            if (auto app = App::GetInstance()) {
                {
                    auto locked = app->GetSettings().LockExclusiveData();
                    locked->StartWithWindows = on;
                }
                app->GetSettings().Save(GetModuleHandleW(nullptr));
            }
            co_return;
        }

        bool success = false;
        if (on) {
            auto state = co_await task.RequestEnableAsync();
            success = (state == winrt::Windows::ApplicationModel::StartupTaskState::Enabled ||
                       state == winrt::Windows::ApplicationModel::StartupTaskState::EnabledByPolicy);
        } else {
            task.Disable();
            success = true;
        }

        co_await ui;
        if (requestId != m_startupRequestId.load()) co_return;

        if (!success) {
            if (auto app = App::GetInstance()) {
                {
                    auto locked = app->GetSettings().LockExclusiveData();
                    locked->StartWithWindows = !on;
                }
                app->GetSettings().Save(GetModuleHandleW(nullptr));
            }
            m_suppressStartupToggle = true;
            StartWithWindowsToggle().IsOn(!on);
            m_suppressStartupToggle = false;
            co_return;
        }

        if (auto app = App::GetInstance()) {
            {
                auto locked = app->GetSettings().LockExclusiveData();
                locked->StartWithWindows = on;
            }
            app->GetSettings().Save(GetModuleHandleW(nullptr));
        }
    } catch (...) {
        revertToggle = true;
    }

    if (revertToggle) {
        co_await ui;
        if (requestId != m_startupRequestId.load()) co_return;
        if (auto app = App::GetInstance()) {
            {
                auto locked = app->GetSettings().LockExclusiveData();
                locked->StartWithWindows = !on;
            }
            app->GetSettings().Save(GetModuleHandleW(nullptr));
        }
        m_suppressStartupToggle = true;
        StartWithWindowsToggle().IsOn(!on);
        m_suppressStartupToggle = false;
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::RebuildDeviceList() {
    DevicesPanel().Children().Clear();

    auto app = App::GetInstance();
    if (!app) return;

    auto secondaryBrush = [&]() -> winrt::Microsoft::UI::Xaml::Media::Brush {
        auto brush = Application::Current().Resources().TryLookup(box_value(L"TextFillColorSecondaryBrush"));
        if (brush) return brush.as<winrt::Microsoft::UI::Xaml::Media::Brush>();
        return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Gray());
    }();

    // Snapshot the device list under a single shared lock, then build
    // UI elements without holding the lock.
    std::vector<DeviceSettings> devices;
    {
        auto locked = app->GetSettings().LockSharedData();
        devices = locked->Devices;
    }

    if (devices.empty()) {
        auto noDevices = TextBlock();
        noDevices.Text(winrt::hstring(_("Settings_NoDevices")));
        noDevices.Foreground(secondaryBrush);
        noDevices.FontSize(12);
        DevicesPanel().Children().Append(noDevices);
        return;
    }

    for (auto& dev : devices) {
        auto item = Grid();
        item.ColumnDefinitions().Append(ColumnDefinition());
        item.ColumnDefinitions().Append(ColumnDefinition());
        item.ColumnDefinitions().Append(ColumnDefinition());
        item.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());
        item.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());

        auto namePanel = StackPanel();
        auto name = TextBlock();
        std::wstring displayName = dev.Name.empty() ? dev.Id : dev.Name;
        name.Text(displayName);
        namePanel.Children().Append(name);

        auto subtitle = TextBlock();
        subtitle.Text(winrt::hstring(_("Settings_PairedDevice")));
        subtitle.Foreground(secondaryBrush);
        subtitle.FontSize(12);
        namePanel.Children().Append(subtitle);

        Grid::SetColumn(namePanel, 0);

        auto toggle = ToggleSwitch();
        toggle.IsOn(dev.AutoReconnect);
        toggle.MinWidth(64);
        toggle.OffContent(box_value(L""));
        toggle.OnContent(box_value(L""));
        ToolTipService::SetToolTip(toggle, box_value(winrt::hstring(_("Device_AutoReconnect"))));
        toggle.Toggled([id = dev.Id](auto const& s, auto) {
            auto app = App::GetInstance();
            if (!app) return;
            bool on = s.template as<ToggleSwitch>().IsOn();
            bool globalAutoReconnect = false;
            {
                auto locked = app->GetSettings().LockExclusiveData();
                for (auto& d : locked->Devices)
                    if (d.Id == id) d.AutoReconnect = on;
                globalAutoReconnect = locked->GlobalAutoReconnect;
            }
            if (auto manager = app->GetDeviceManager()) {
                manager->SetAutoReconnect(winrt::hstring(id), globalAutoReconnect || on);
            }
            app->GetSettings().Save(GetModuleHandleW(nullptr));
        });
        Grid::SetColumn(toggle, 1);

        auto forgetBtn = Button();
        forgetBtn.MinWidth(80);
        forgetBtn.Content(box_value(winrt::hstring(_("Device_Forget"))));
        auto weak = get_weak();
        forgetBtn.Click([id = dev.Id, weak](auto, auto) {
            auto app = App::GetInstance();
            if (!app) return;
            {
                auto locked = app->GetSettings().LockExclusiveData();
                auto& vec = locked->Devices;
                std::erase_if(vec, [&](auto const& d) { return d.Id == id; });
            }
            app->GetSettings().Save(GetModuleHandleW(nullptr));
            if (auto self = weak.get()) {
                self->RebuildDeviceList();
            }
        });
        Grid::SetColumn(forgetBtn, 2);

        item.Children().Append(namePanel);
        item.Children().Append(toggle);
        item.Children().Append(forgetBtn);
        DevicesPanel().Children().Append(item);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindow::SetTargetPosition(int32_t x, int32_t y) {
    m_targetX = x;
    m_targetY = y;
}

} // namespace winrt::AudioPlaybackConnector2::implementation
