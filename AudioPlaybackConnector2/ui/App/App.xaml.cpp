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

namespace {
TrayNotificationType ToTrayNotificationType(NotificationService::FallbackNotificationType type) {
    switch (type) {
        case NotificationService::FallbackNotificationType::Warning: return TrayNotificationType::Warning;
        case NotificationService::FallbackNotificationType::Error: return TrayNotificationType::Error;
        case NotificationService::FallbackNotificationType::Info:
        default: return TrayNotificationType::Info;
    }
}
} // namespace

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
    m_notificationService.reset();
    m_trayController.reset();
    s_instance.store(nullptr);
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e) {
    m_singleInstanceMutex.reset(CreateMutexW(nullptr, TRUE, L"AudioPlaybackConnector2_SingleInstance_v2"));
    if (!m_singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        ExitProcess(0);
        return;
    }
    DebugTrace(L"[App] OnLaunched started");
    SetupMainWindow();
}

void winrt::AudioPlaybackConnector2::implementation::App::SetupMainWindow() {
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
        OnMainWindowLoaded(root);
    });

    m_mainWindow.Activate();
    DebugTrace(L"[App] MainWindow.Activate() called");
}

void winrt::AudioPlaybackConnector2::implementation::App::OnMainWindowLoaded(Controls::Grid const& root) {
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

    InitializeDeviceManager();
    InitializeTray();
    InitializeNotifications();
    SetupDeviceEvents();
    if (m_notificationService) {
        m_notificationService->ShowAppStarted();
    }
    TryAutoReconnect();

    gdiplusGuard.release();

    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    m_trayController->UpdateTooltipFromConnections();
    DebugTrace(L"[App] Initialization complete");
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Initializers /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::InitializeTray() {
    DebugTrace(L"[App] InitializeTray()");
    m_trayController = std::make_unique<TrayController>();
    m_trayController->Initialize(m_hwnd, m_mainWindow);
    m_trayController->SetDeviceManager(m_deviceManager);
    auto weak = get_weak();
    m_trayController->SetCallbacks(
        [weak]() {
            if (auto self = weak.get()) self->ShowSettingsWindow();
        },
        [weak]() {
            if (auto self = weak.get()) self->ExitApplication();
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->ConnectAsync(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->Disconnect(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->ReconnectAsync(id);
        });
    m_trayController->PreloadDevicePicker();
    DebugTrace(L"[App] TrayController initialized");
}

void winrt::AudioPlaybackConnector2::implementation::App::InitializeNotifications() {
    DebugTrace(L"[App] InitializeNotifications()");
    m_notificationService = std::make_unique<NotificationService>();
    auto weak = get_weak();
    m_notificationService->SetReconnectCallback([weak](winrt::hstring deviceId) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, deviceId = std::move(deviceId)]() mutable {
                if (auto self = weak.get()) {
                    if (self->m_exiting.load() || !self->m_deviceManager) return;
                    self->m_deviceManager->ReconnectAsync(deviceId);
                }
            });
        }
    });
    m_notificationService->SetFallbackNotifier([weak](std::wstring const& title, std::wstring const& body, NotificationService::FallbackNotificationType type) {
        if (auto self = weak.get()) {
            if (self->m_exiting.load() || !self->m_trayController) return;
            self->m_trayController->ShowNotification(title, body, ToTrayNotificationType(type));
        }
    });
    m_notificationsAvailable = m_notificationService->Initialize(winrt::hstring(_("AppName")), winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
    DebugTrace(L"[App] Notifications available: {0}", m_notificationsAvailable);
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
        return std::any_of(locked->Devices.begin(), locked->Devices.end(), [&](const auto& d) { return d.Id == id && d.AutoReconnect; });
    });
    m_deviceManager->StartDeviceWatcher();
    DebugTrace(L"[App] DeviceManager initialized and watcher started");
}

winrt::hstring winrt::AudioPlaybackConnector2::implementation::App::ResolveKnownDeviceName(winrt::hstring const& id) const {
    if (!m_settings) return id;
    auto locked = m_settings->LockSharedData();
    auto it = std::find_if(locked->Devices.begin(), locked->Devices.end(), [&](const auto& device) { return device.Id == id; });
    if (it != locked->Devices.end()) return winrt::hstring(it->Name);
    return id;
}

void winrt::AudioPlaybackConnector2::implementation::App::TryAutoReconnect() {
    if (m_exiting.load() || !m_settings || !m_deviceManager) return;

    DebugTrace(L"[App] TryAutoReconnect()");
    bool globalAutoReconnect = false;
    std::vector<std::wstring> lastConnectedIds;
    std::vector<DeviceSettings> devices;
    {
        auto locked = m_settings->LockSharedData();
        globalAutoReconnect = locked->GlobalAutoReconnect;
        lastConnectedIds = locked->LastConnectedIds;
        devices = locked->Devices;
    }

    for (const auto& id : lastConnectedIds) {
        auto device = std::find_if(devices.begin(), devices.end(), [&](const auto& knownDevice) { return knownDevice.Id == id; });
        if (device != devices.end() && (globalAutoReconnect || device->AutoReconnect)) {
            DebugTrace(L"[App] Auto-reconnecting to: {0}", id);
            m_deviceManager->ConnectAsync(winrt::hstring(id));
        }
    }
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
    m_autoReconnectFailedToken = m_deviceManager->AutoReconnectFailed += [weak](auto id) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id]() {
                if (auto self = weak.get()) self->OnAutoReconnectFailed(id);
            });
        }
    };
    m_deviceStatusChangedToken = m_deviceManager->DeviceStatusChanged += [weak](auto id, auto status, auto) {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak, id, status]() {
                auto self = weak.get();
                if (!self) return;
                if (self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
                if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
                if (status == winrt::hstring(_("Connecting")) || status == winrt::hstring(_("Reconnecting"))) {
                    self->m_trayController->SetState(TrayIconState::Connecting);
                    SetTimer(self->m_hwnd, c_timerAnimation, 200, nullptr);
                    self->m_trayController->UpdateTooltipFromConnections();
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
                    self->m_trayController->SetState(TrayIconState::Connected);
                } else if (!status.empty()) {
                    KillTimer(self->m_hwnd, c_timerAnimation);
                    self->m_trayController->SetState(self->m_deviceManager->HasConnections() ? TrayIconState::Connected : TrayIconState::Error);
                    self->m_trayController->UpdateTooltipFromConnections();
                } else {
                    KillTimer(self->m_hwnd, c_timerAnimation);
                    self->m_trayController->SetState(self->m_deviceManager->HasConnections() ? TrayIconState::Connected : TrayIconState::Idle);
                    self->m_trayController->UpdateTooltipFromConnections();
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
    if (m_autoReconnectFailedToken) {
        m_deviceManager->AutoReconnectFailed -= m_autoReconnectFailedToken;
        m_autoReconnectFailedToken = 0;
    }
    if (m_deviceStatusChangedToken) {
        m_deviceManager->DeviceStatusChanged -= m_deviceStatusChangedToken;
        m_deviceStatusChangedToken = 0;
    }
}

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

        auto impl = m_settingsWindow.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        if (impl && m_trayController) {
            if (auto pos = m_trayController->GetSettingsWindowPosition()) {
                impl->SetTargetPosition(pos->x, pos->y);
            }
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
    m_notificationService.reset();

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

    m_trayController.reset();
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

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Handlers ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnDeviceConnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_deviceManager || !m_notificationService || !m_trayController) return;
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
        auto it = std::find_if(locked->Devices.begin(), locked->Devices.end(), [&](const auto& d) { return d.Id == id; });
        if (it != locked->Devices.end())
            autoReconnect = autoReconnect || it->AutoReconnect;
        m_deviceManager->SetAutoReconnect(id, autoReconnect);
    }

    m_notificationService->ShowDeviceConnected(id, deviceName);

    m_trayController->UpdateTooltipFromConnections();
}

void winrt::AudioPlaybackConnector2::implementation::App::OnDeviceDisconnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_notificationService || !m_trayController) return;
    DebugTrace(L"[App] OnDeviceDisconnected: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    m_notificationService->ShowDeviceDisconnected(id, deviceName);

    m_trayController->UpdateTooltipFromConnections();
}

void winrt::AudioPlaybackConnector2::implementation::App::OnConnectionError(winrt::hstring const& id, winrt::hstring msg) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnConnectionError: {0} - {1}", std::wstring(id), std::wstring(msg));
    if (m_trayController) {
        std::wstring tip = std::wstring(_("AppName")) + L"\n" + std::wstring(msg);
        m_trayController->UpdateTooltip(tip);
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::OnAutoReconnectTriggered(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_notificationService) return;
    DebugTrace(L"[App] OnAutoReconnectTriggered: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    m_notificationService->ShowAutoReconnect(id, deviceName);
}

void winrt::AudioPlaybackConnector2::implementation::App::OnAutoReconnectFailed(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_notificationService) return;
    DebugTrace(L"[App] OnAutoReconnectFailed: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    m_notificationService->ShowAutoReconnectFailed(id, deviceName);
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Window Subclass //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

LRESULT CALLBACK winrt::AudioPlaybackConnector2::implementation::App::SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) {
    auto* app = reinterpret_cast<App*>(dwRefData);
    if (!app) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (app->m_trayController && msg == app->m_trayController->TrayCallbackMessage()) {
        app->m_trayController->HandleTrayMessage(wParam, lParam);
        return 0;
    }

    if (msg == WM_SETTINGCHANGE) {
        ThemeHelper::OnSettingChange(hwnd, lParam);
        return 0;
    }

    if (msg == WM_TIMER && wParam == c_timerAnimation && app->m_trayController) {
        app->m_trayController->ToggleConnectingFrame();
        return 0;
    }

    if (s_wmTaskbarCreated && msg == s_wmTaskbarCreated) {
        if (app->m_trayController) {
            app->m_trayController->Reregister();
            app->m_trayController->OnThemeChanged();
        }
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
