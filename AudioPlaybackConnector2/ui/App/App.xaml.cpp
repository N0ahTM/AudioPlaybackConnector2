#include <pch.h>
#include <ui/App/App.xaml.h>
#include <MainWindow/MainWindow.xaml.h>
#include <SettingsWindow/SettingsWindow.xaml.h>
#include <DevicePickerView/DevicePickerView.xaml.h>

#include <ui/TrayIcon.hpp>
#include <ui/TrayContextMenu.hpp>
#include <core/DeviceManager.hpp>
#include <core/Settings.hpp>
#include <core/ThemeHelper.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

/* Helpers */
/*------------------------------------------------------------------------------------------------------------------*/

static std::wstring ReplaceFirstPlaceholder(std::wstring_view templateStr, std::wstring_view replacement) {
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

/* Member Variables */
/*------------------------------------------------------------------------------------------------------------------*/

namespace winrt::AudioPlaybackConnector2::implementation {
std::atomic<App*> App::s_instance = nullptr;
} // namespace winrt::AudioPlaybackConnector2::implementation

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

winrt::AudioPlaybackConnector2::implementation::App::App() {
    s_instance.store(this);
}

winrt::AudioPlaybackConnector2::implementation::App::~App() {
    TeardownDeviceEvents();
    if (m_themeChangedToken) {
        ThemeHelper::RemoveThemeChangedHandler(m_themeChangedToken);
        m_themeChangedToken = 0;
    }
    s_instance.store(nullptr);
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e) {
    DebugTrace(L"[App] OnLaunched started");

    m_mainWindow = make<MainWindow>();
    m_dispatcherQueue = m_mainWindow.DispatcherQueue();

    // Move the window off-screen before Activate() to prevent any visible flash.
    auto appWindow = m_mainWindow.AppWindow();
    if (appWindow) {
        appWindow.Move({-32000, -32000});
        appWindow.Resize({1, 1});
    }

    auto content = m_mainWindow.Content();
    if (!content) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is null!");
        return;
    }

    auto root = content.as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is not a Grid!");
        return;
    }

    // Hide the Grid until the window is positioned off-screen to avoid
    // a visible black/white flash during startup.
    root.Opacity(0);

    // Use Grid.Loaded instead of Window.Activated.
    // Loaded fires after the element is added to the visual tree and XamlRoot
    // has been assigned, which is required for MenuFlyout anchoring.
    root.Loaded([this, root](auto&, auto&) {
        if (m_hwnd) {
            DebugTrace(L"[App] Grid.Loaded fired again, ignoring (already initialized)");
            return;
        }
        DebugTrace(L"[App] Grid.Loaded - beginning initialization");

        m_hwnd = util::GetWindowHandle(m_mainWindow);
        if (!m_hwnd) {
            DebugTrace(L"[App] ERROR: GetWindowHandle returned null!");
            return;
        }
        DebugTrace(L"[App] MainWindow HWND = 0x{0:X}", reinterpret_cast<uintptr_t>(m_hwnd));

        auto exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
        ShowWindow(m_hwnd, SW_SHOWNA);
        root.Opacity(1);
        DebugTrace(L"[App] MainWindow moved off-screen (toolwindow, showna)");

        SetWindowSubclass(m_hwnd, SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        DebugTrace(L"[App] Window subclass installed");

        m_settings = std::make_unique<::Settings>();
        m_settings->Load(GetModuleHandleW(nullptr));
        DebugTrace(L"[App] Settings loaded");

        {
            auto locked = m_settings->LockSharedData();
            StringResources::Instance().Initialize(GetModuleHandleW(nullptr), locked->Language);
        }
        DebugTrace(L"[App] StringResources initialized");

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
            DebugTrace(L"[App] ERROR: GdiplusStartup failed");
            return;
        }
        DebugTrace(L"[App] GDI+ initialized");
        auto gdiplusGuard = wil::scope_exit([this]() {
            if (m_gdiplusToken) {
                Gdiplus::GdiplusShutdown(m_gdiplusToken);
                m_gdiplusToken = 0;
            }
        });

        InitializeTray();
        InitializeDeviceManager();
        InitializeContextMenu();
        PreloadDevicePicker();
        SetupDeviceEvents();
        TryAutoReconnect();

        gdiplusGuard.release();

        s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        UpdateTrayTooltip();
        DebugTrace(L"[App] Initialization complete");
    });

    m_mainWindow.Activate();
    DebugTrace(L"[App] MainWindow.Activate() called");
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Initializers /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::InitializeTray() {
    DebugTrace(L"[App] InitializeTray()");
    m_trayIcon = std::make_unique<TrayIcon>();
    m_trayIcon->Initialize(m_hwnd, m_trayCallbackMsg);
    m_trayIcon->UpdateTheme(ThemeHelper::IsSystemLightTheme());
    DebugTrace(L"[App] TrayIcon initialized, theme = {0}", ThemeHelper::IsSystemLightTheme() ? L"light" : L"dark");

    m_themeChangedToken = ThemeHelper::AddThemeChangedHandler([this](bool light) {
        if (m_exiting.load() || !m_trayIcon) return;
        DebugTrace(L"[App] Theme changed to {0}", light ? L"light" : L"dark");
        m_trayIcon->UpdateTheme(light);
    });
}

void winrt::AudioPlaybackConnector2::implementation::App::InitializeDeviceManager() {
    DebugTrace(L"[App] InitializeDeviceManager()");
    m_deviceManager = std::make_shared<DeviceManager>();
    auto weak = get_weak();
    m_deviceManager->SetAutoReconnectPredicate([weak](auto id) {
        auto self = weak.get();
        if (!self || self->m_exiting.load() || !self->m_settings) return false;
        auto locked = self->m_settings->LockSharedData();
        if (locked->GlobalAutoReconnect) return true;
        for (const auto& d : locked->Devices)
            if (d.Id == id && d.AutoReconnect) return true;
        return false;
    });
    m_deviceManager->StartDeviceWatcher();
    DebugTrace(L"[App] DeviceManager initialized and watcher started");
}

void winrt::AudioPlaybackConnector2::implementation::App::InitializeContextMenu() {
    DebugTrace(L"[App] InitializeContextMenu()");
    if (!m_mainWindow.Content()) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is null!");
        return;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is not a Grid!");
        return;
    }

    if (!root.XamlRoot()) {
        DebugTrace(L"[App] ERROR: Grid.XamlRoot() is null! ContextMenu will fail.");
        return;
    }
    DebugTrace(L"[App] Grid.XamlRoot() is valid");

    m_contextMenu = std::make_unique<TrayContextMenu>();
    m_contextMenu->Initialize(root, [this]() { ShowSettingsWindow(); }, []() {
            auto op = winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:bluetooth"));
            op.Completed([](auto const& sender, auto const&) {
                if (!sender.GetResults()) {
                    DebugTrace(L"[App] LaunchUriAsync(ms-settings:bluetooth) failed");
                }
            }); }, [this]() { ExitApplication(); }, [this]() {
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } });
    DebugTrace(L"[App] TrayContextMenu initialized");
}

void winrt::AudioPlaybackConnector2::implementation::App::RunOnUIThread(std::function<void()> work) {
    if (m_exiting.load()) return;
    if (!m_dispatcherQueue) return;
    if (m_dispatcherQueue.HasThreadAccess()) {
        if (m_exiting.load()) return;
        work();
    } else {
        auto weak = get_weak();
        m_dispatcherQueue.TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
            [weak, work = std::move(work)]() {
                auto self = weak.get();
                if (!self || self->m_exiting.load()) return;
                work();
            });
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::SetupDeviceEvents() {
    DebugTrace(L"[App] SetupDeviceEvents()");
    auto weak = get_weak();
    m_deviceConnectedToken = m_deviceManager->DeviceConnected += [weak](auto id) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id]() {
                if (auto self = weak.get()) self->OnDeviceConnected(id);
            });
        }
    };
    m_deviceDisconnectedToken = m_deviceManager->DeviceDisconnected += [weak](auto id) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id]() {
                if (auto self = weak.get()) self->OnDeviceDisconnected(id);
            });
        }
    };
    m_connectionErrorToken = m_deviceManager->ConnectionError += [weak](auto id, auto msg) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id, msg]() {
                if (auto self = weak.get()) self->OnConnectionError(id, msg);
            });
        }
    };
    m_autoReconnectTriggeredToken = m_deviceManager->AutoReconnectTriggered += [weak](auto id) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id]() {
                if (auto self = weak.get()) self->OnAutoReconnectTriggered(id);
            });
        }
    };
    m_deviceStatusChangedToken = m_deviceManager->DeviceStatusChanged += [weak](auto id, auto status, auto) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id, status]() {
                auto self = weak.get();
                if (!self) return;
                if (self->m_exiting.load() || !self->m_trayIcon || !self->m_deviceManager) return;
                if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
                if (status == winrt::hstring(_("Connecting")) || status == winrt::hstring(_("Reconnecting"))) {
                    self->m_trayIcon->SetState(TrayIconState::Connecting);
                    SetTimer(self->m_hwnd, c_timerAnimation, 200, nullptr);
                    self->UpdateTrayTooltip();
                } else if (status == winrt::hstring(_("Connected"))) {
                    bool isStillConnected = false;
                    for (const auto& c : self->m_deviceManager->GetConnectedDevices()) {
                        if (c.Device.Id() == id) {
                            isStillConnected = true;
                            break;
                        }
                    }
                    if (!isStillConnected) return;
                    KillTimer(self->m_hwnd, c_timerAnimation);
                    self->m_trayIcon->SetState(TrayIconState::Connected);
                } else if (!status.empty()) {
                    KillTimer(self->m_hwnd, c_timerAnimation);
                    self->m_trayIcon->SetState(TrayIconState::Error);
                } else {
                    KillTimer(self->m_hwnd, c_timerAnimation);
                    self->m_trayIcon->SetState(TrayIconState::Idle);
                    self->UpdateTrayTooltip();
                }
            });
        }
    };
}

void winrt::AudioPlaybackConnector2::implementation::App::TeardownDeviceEvents() {
    if (!m_deviceManager) return;
    if (m_deviceConnectedToken) {
        m_deviceManager->DeviceConnected -= m_deviceConnectedToken;
        m_deviceConnectedToken = 0;
    }
    if (m_deviceDisconnectedToken) {
        m_deviceManager->DeviceDisconnected -= m_deviceDisconnectedToken;
        m_deviceDisconnectedToken = 0;
    }
    if (m_connectionErrorToken) {
        m_deviceManager->ConnectionError -= m_connectionErrorToken;
        m_connectionErrorToken = 0;
    }
    if (m_autoReconnectTriggeredToken) {
        m_deviceManager->AutoReconnectTriggered -= m_autoReconnectTriggeredToken;
        m_autoReconnectTriggeredToken = 0;
    }
    if (m_deviceStatusChangedToken) {
        m_deviceManager->DeviceStatusChanged -= m_deviceStatusChangedToken;
        m_deviceStatusChangedToken = 0;
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::TryAutoReconnect() {
    DebugTrace(L"[App] TryAutoReconnect()");
    bool globalAutoReconnect = false;
    std::vector<std::wstring> lastConnectedIds;
    {
        auto locked = m_settings->LockSharedData();
        globalAutoReconnect = locked->GlobalAutoReconnect;
        lastConnectedIds = locked->LastConnectedIds;
    }
    for (const auto& id : lastConnectedIds) {
        bool shouldReconnect = false;
        {
            auto locked = m_settings->LockSharedData();
            for (const auto& d : locked->Devices) {
                if (d.Id == id) {
                    shouldReconnect = globalAutoReconnect || d.AutoReconnect;
                    break;
                }
            }
        }
        if (shouldReconnect) {
            DebugTrace(L"[App] Auto-reconnecting to: {0}", id);
            m_deviceManager->ConnectAsync(winrt::hstring(id));
        }
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Actions /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::ShowSettingsWindow() {
    DebugTrace(L"[App] ShowSettingsWindow()");
    if (m_settingsWindow) {
        auto hwnd = util::GetWindowHandle(m_settingsWindow);
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            DebugTrace(L"[App] SettingsWindow brought to foreground");
        }
        return;
    }

    try {
        m_settingsWindow = winrt::AudioPlaybackConnector2::SettingsWindow();

        // Calculate position relative to tray icon before styling is applied.
        int targetX = INT_MIN;
        int targetY = INT_MIN;
        if (m_trayIcon) {
            auto rect = m_trayIcon->GetIconRect();
            if (rect) {
                RECT rcWork{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

                targetX = rect->right - c_settingsWindowWidth; // right-align to icon
                targetY = rect->bottom;                        // below the icon

                // Keep within horizontal work area bounds.
                if (targetX < rcWork.left) targetX = rcWork.left;
                if (targetX + c_settingsWindowWidth > rcWork.right) targetX = rcWork.right - c_settingsWindowWidth;

                // If it doesn't fit below the taskbar, open above the icon.
                if (targetY + c_settingsWindowHeight > rcWork.bottom) {
                    targetY = rect->top - c_settingsWindowHeight;
                }

                // Final safety fallback.
                if (targetY < rcWork.top) {
                    targetY = rcWork.bottom - c_settingsWindowHeight;
                }
            }
        }

        auto impl = m_settingsWindow.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        if (impl && targetX != INT_MIN) {
            impl->SetTargetPosition(targetX, targetY);
        }

        // Move off-screen BEFORE Activate() so the window never appears
        // on screen until RootGrid_Loaded has finished styling and content.
        auto appWindow = m_settingsWindow.AppWindow();
        if (appWindow) {
            appWindow.Move({-32000, -32000});
            appWindow.Resize({1, 1});
        }

        m_settingsWindow.Activate();

        auto hwnd = util::GetWindowHandle(m_settingsWindow);
        if (hwnd) {
            ShowWindow(hwnd, SW_HIDE);
        }

        m_settingsWindow.Closed([this](auto&, auto&) {
            DebugTrace(L"[App] SettingsWindow closed");
            m_settingsWindow = nullptr;
            if (m_settings) {
                m_settings->Save(GetModuleHandleW(nullptr));
            }
        });
        DebugTrace(L"[App] SettingsWindow created off-screen (hidden until ready)");
    } catch (...) {
        DebugTrace(L"[App] ERROR: Failed to create SettingsWindow");
        m_settingsWindow = nullptr;
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::ExitApplication() {
    if (m_exiting.exchange(true)) return;
    DebugTrace(L"[App] ExitApplication() started");

    if (m_deviceManager) {
        m_deviceManager->StopDeviceWatcher();
        m_deviceManager->CancelPendingReconnects();
    }

    // Don't synchronously Close() connections on exit – that can block the UI thread.
    // The OS will clean up the underlying Bluetooth handles when the process terminates.
    if (m_settings && m_deviceManager) {
        {
            auto locked = m_settings->LockExclusiveData();
            locked->LastConnectedIds.clear();
            for (const auto& c : m_deviceManager->GetConnectedDevices())
                locked->LastConnectedIds.push_back(std::wstring(c.Device.Id()));
        }
        m_settings->Save(GetModuleHandleW(nullptr));
    }

    if (m_trayIcon) m_trayIcon->Remove();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    if (m_settingsWindow) {
        try {
            m_settingsWindow.Close();
        } catch (...) {
        }
    }

    if (m_hwnd) {
        RemoveWindowSubclass(m_hwnd, SubclassProc, 1);
    }
    m_mainWindow.Close();
    DebugTrace(L"[App] ExitApplication() complete");
}

void winrt::AudioPlaybackConnector2::implementation::App::UpdateTrayTooltip() {
    if (m_exiting.load() || !m_trayIcon || !m_deviceManager) return;
    auto connected = m_deviceManager->GetConnectedDevices();
    if (connected.empty()) {
        m_trayIcon->SetTooltip(_("AppName"));
    } else {
        std::wstring tip = std::wstring(_("AppName")) + L"\n";
        for (const auto& c : connected) {
            tip += c.Device.Name();
            tip += L"\n";
        }
        m_trayIcon->SetTooltip(tip);
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Handlers ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::EnsureDevicePickerViewCreated() {
    if (m_devicePickerView) {
        return;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[App] ERROR: EnsureDevicePickerViewCreated failed, Content() is not a Grid");
        return;
    }
    if (!root.XamlRoot()) {
        DebugTrace(L"[App] ERROR: EnsureDevicePickerViewCreated failed, XamlRoot() is null");
        return;
    }

    m_devicePickerView = winrt::AudioPlaybackConnector2::DevicePickerView();
    auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
    impl->Initialize(m_deviceManager, [this]() { if (m_pickerFlyout) m_pickerFlyout.Hide(); }, [this](winrt::hstring id) {
            DebugTrace(L"[App] User selected device: {0}", std::wstring(id));
            m_deviceManager->ConnectAsync(id);
            if (m_pickerFlyout) m_pickerFlyout.Hide(); }, [this](winrt::hstring id) {
            DebugTrace(L"[App] User disconnected device: {0}", std::wstring(id));
            if (m_pickerFlyout) m_pickerFlyout.Hide();
            m_deviceManager->Disconnect(id); }, [this](winrt::hstring id) {
            DebugTrace(L"[App] User reconnected device: {0}", std::wstring(id));
            m_deviceManager->ReconnectAsync(id); });
}

void winrt::AudioPlaybackConnector2::implementation::App::PreloadDevicePicker() {
    if (m_devicePickerPreloaded) {
        return;
    }
    m_devicePickerPreloaded = true;

    EnsureDevicePickerViewCreated();

    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->LoadDevices();
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::OnTrayIconLeftClick() {
    DebugTrace(L"[App] OnTrayIconLeftClick()");

    auto rect = m_trayIcon->GetIconRect();
    if (!rect) {
        DebugTrace(L"[App] ERROR: GetIconRect() returned null");
        return;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is not a Grid");
        return;
    }

    if (m_pickerFlyout && m_pickerFlyout.IsOpen()) {
        DebugTrace(L"[App] Picker flyout already open, ignoring second click");
        return;
    }

    EnsureDevicePickerViewCreated();

    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->LoadDevices();
    }

    // Create a fresh Flyout each time to avoid WinUI 3 reuse exceptions.
    // The DevicePickerView (expensive) is reused; the Flyout (cheap) is recreated.
    m_pickerFlyout = Controls::Flyout();
    m_pickerFlyout.ShouldConstrainToRootBounds(false);
    m_pickerFlyout.Content(m_devicePickerView);

    m_pickerFlyout.Opened([this](auto&, auto&) {
        try {
            auto content = m_pickerFlyout.Content();
            auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(content);
            while (parent) {
                auto presenter = parent.try_as<Controls::FlyoutPresenter>();
                if (presenter) {
                    presenter.Background(nullptr);
                    presenter.BorderBrush(nullptr);
                    presenter.BorderThickness({0});
                    presenter.Padding({0});
                    presenter.MinWidth(0);
                    presenter.MinHeight(0);
                    break;
                }
                parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(parent);
            }
        } catch (...) {
        }
    });

    m_pickerFlyout.Closing([this](auto&, auto&) {
        try {
            // Cancel any in-flight device enumeration before detaching.
            if (m_devicePickerView) {
                auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
                impl->CancelLoadDevices();
            }
            // Detach the reusable view before the flyout closes so WinUI 3
            // does not touch (and potentially dispose) our cached content.
            if (m_pickerFlyout) {
                m_pickerFlyout.Content(nullptr);
            }
        } catch (...) {
        }
    });

    m_pickerFlyout.Closed([this](auto&, auto&) {
        DebugTrace(L"[App] Picker flyout closed");
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        m_pickerFlyout = nullptr;
    });
    POINT pt{rect->left, rect->top};
    ScreenToClient(m_hwnd, &pt);
    auto dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    winrt::Windows::Foundation::Point point(
        static_cast<float>(pt.x) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi),
        static_cast<float>(pt.y) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi));

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(m_hwnd);

    Controls::Primitives::FlyoutShowOptions options;
    options.Position(point);
    m_pickerFlyout.ShowAt(root, options);
}

void winrt::AudioPlaybackConnector2::implementation::App::OnTrayIconRightClick() {
    DebugTrace(L"[App] OnTrayIconRightClick()");
    if (!m_contextMenu) {
        DebugTrace(L"[App] ERROR: m_contextMenu is null");
        return;
    }

    POINT pt{};
    GetCursorPos(&pt);

    // MenuFlyout.ShowAt expects coordinates relative to the anchor element.
    // Since the window is off-screen we must convert Screen -> Client.
    ScreenToClient(m_hwnd, &pt);

    auto dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    SetForegroundWindow(m_hwnd);

    winrt::Windows::Foundation::Point point(
        static_cast<float>(pt.x) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi),
        static_cast<float>(pt.y) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi));

    DebugTrace(L"[App] ContextMenu showing at ({0}, {1}) with DPI={2}", point.X, point.Y, dpi);

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    m_contextMenu->ShowAt(point);
}

void winrt::AudioPlaybackConnector2::implementation::App::OnTrayIconMessage([[maybe_unused]] WPARAM wParam, LPARAM lParam) {
    auto loword = LOWORD(lParam);

    constexpr ULONGLONG c_clickDebounceMs = 200;
    static ULONGLONG s_lastLeftClickTick = 0;
    static ULONGLONG s_lastRightClickTick = 0;

    auto shouldProcess = [](ULONGLONG& lastTick, ULONGLONG debounceMs) {
        auto now = GetTickCount64();
        if (now - lastTick < debounceMs) {
            return false;
        }
        lastTick = now;
        return true;
    };

    switch (loword) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (shouldProcess(s_lastLeftClickTick, c_clickDebounceMs)) {
                OnTrayIconLeftClick();
            }
            break;

        // Prefer WM_CONTEXTMENU for right-click to avoid duplicate handling with WM_RBUTTONUP.
        case WM_CONTEXTMENU:
            if (shouldProcess(s_lastRightClickTick, c_clickDebounceMs)) {
                OnTrayIconRightClick();
            }
            break;

        case WM_RBUTTONUP:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MOUSEMOVE:
        case NIN_BALLOONSHOW:
        case NIN_BALLOONHIDE:
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONUSERCLICK:
            // Expected shell notifications/noise.
            break;

        default:
            DebugTrace(L"[App] Unhandled tray message: 0x{0:X}", loword);
            break;
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Handlers ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnDeviceConnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_deviceManager) return;
    DebugTrace(L"[App] OnDeviceConnected: {0}", std::wstring(id));

    // Resolve device name before acquiring the settings lock to avoid lock ordering issues.
    winrt::hstring deviceName = id;
    bool isStillConnected = false;
    for (const auto& c : m_deviceManager->GetConnectedDevices()) {
        if (c.Device.Id() == id) {
            deviceName = c.Device.Name();
            isStillConnected = true;
            break;
        }
    }
    if (!isStillConnected) return;

    // Check-and-insert under a single exclusive lock to prevent duplicate entries.
    bool addedNew = false;
    {
        auto locked = m_settings->LockExclusiveData();
        bool alreadyKnown = std::any_of(locked->Devices.begin(), locked->Devices.end(), [&](const auto& d) { return d.Id == id; });
        if (!alreadyKnown) {
            DeviceSettings newDevice;
            newDevice.Id = std::wstring(id);
            newDevice.Name = std::wstring(deviceName);
            newDevice.AutoReconnect = locked->GlobalAutoReconnect;
            locked->Devices.push_back(std::move(newDevice));
            addedNew = true;
        }
    }

    if (addedNew) {
        m_settings->Save(GetModuleHandleW(nullptr));
        DebugTrace(L"[App] New device added to settings: {0}", std::wstring(deviceName));
    }

    {
        auto locked = m_settings->LockSharedData();
        bool autoReconnect = locked->GlobalAutoReconnect;
        for (const auto& d : locked->Devices) {
            if (d.Id == id) {
                autoReconnect = autoReconnect || d.AutoReconnect;
                break;
            }
        }
        m_deviceManager->SetAutoReconnect(id, autoReconnect);
    }

    if (m_trayIcon) {
        auto msg = ReplaceFirstPlaceholder(std::wstring(_("Notification_Connected")), deviceName);
        m_trayIcon->ShowNotification(_("AppName"), msg, TrayNotificationType::Info);
    }

    UpdateTrayTooltip();
}

void winrt::AudioPlaybackConnector2::implementation::App::OnDeviceDisconnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings) return;
    DebugTrace(L"[App] OnDeviceDisconnected: {0}", std::wstring(id));

    winrt::hstring deviceName = id;
    {
        auto locked = m_settings->LockSharedData();
        for (const auto& d : locked->Devices) {
            if (d.Id == id) {
                deviceName = winrt::hstring(d.Name);
                break;
            }
        }
    }

    if (m_trayIcon) {
        auto msg = ReplaceFirstPlaceholder(std::wstring(_("Notification_Disconnected")), deviceName);
        m_trayIcon->ShowNotification(_("AppName"), msg, TrayNotificationType::Warning);
    }

    UpdateTrayTooltip();
}

void winrt::AudioPlaybackConnector2::implementation::App::OnConnectionError(winrt::hstring const& id, winrt::hstring msg) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnConnectionError: {0} - {1}", std::wstring(id), std::wstring(msg));
    if (m_trayIcon) {
        std::wstring tip = std::wstring(_("AppName")) + L"\n" + msg.c_str();
        m_trayIcon->SetTooltip(tip);
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::OnAutoReconnectTriggered(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings) return;
    DebugTrace(L"[App] OnAutoReconnectTriggered: {0}", std::wstring(id));

    winrt::hstring deviceName = id;
    {
        auto locked = m_settings->LockSharedData();
        for (const auto& d : locked->Devices) {
            if (d.Id == id) {
                deviceName = winrt::hstring(d.Name);
                break;
            }
        }
    }

    if (m_trayIcon) {
        auto msg = ReplaceFirstPlaceholder(std::wstring(_("Notification_AutoReconnect")), deviceName);
        m_trayIcon->ShowNotification(_("AppName"), msg, TrayNotificationType::Info);
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Window Subclass //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

LRESULT CALLBACK winrt::AudioPlaybackConnector2::implementation::App::SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) {
    auto* app = reinterpret_cast<App*>(dwRefData);
    if (!app) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == app->m_trayCallbackMsg) {
        app->OnTrayIconMessage(wParam, lParam);
        return 0;
    }

    if (msg == WM_SETTINGCHANGE) {
        ThemeHelper::OnSettingChange(hwnd, lParam);
        return 0;
    }

    if (msg == WM_TIMER && wParam == c_timerAnimation && app->m_trayIcon) {
        app->m_trayIcon->ToggleConnectingFrame();
        return 0;
    }

    if (s_wmTaskbarCreated && msg == s_wmTaskbarCreated) {
        if (app->m_trayIcon) {
            app->m_trayIcon->Reregister();
            app->m_trayIcon->UpdateTheme(ThemeHelper::IsSystemLightTheme());
        }
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
