#include <pch.h>
#include <ui/App/App.xaml.h>
#include <MainWindow/MainWindow.xaml.h>
#include <DevicePickerView/DevicePickerView.xaml.h>

#include <app/StartupUpdateCoordinator.hpp>
#include <ui/TrayIcon.hpp>
#include <ui/TrayContextMenu.hpp>
#include <core/DeviceManager.hpp>
#include <core/Settings.hpp>
#include <core/ThemeHelper.hpp>
#include <core/StringResources.hpp>
#include <services/SettingsController.hpp>
#include <util/CrashHandler.hpp>
#include <util/Util.hpp>

#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::AudioPlaybackConnector2::implementation::App::App() {
    util::crash::InstallCrashHandlers();
}

winrt::AudioPlaybackConnector2::implementation::App::~App() {
    m_exiting.store(true);
    m_powerTransitionCoordinator.Cancel();
    m_settingsWindowPresenter.Close();
    TeardownDeviceEvents();
    if (m_hwnd) {
        try {
            KillTimer(m_hwnd, c_timerAnimation);
            RemoveWindowSubclass(m_hwnd, SubclassProc, 1);
        } catch (...) {
        }
    }
    if (m_trayController) {
        m_trayController->Teardown();
    }
    if (m_notificationService) {
        m_notificationService->Teardown();
    }
    if (m_deviceManager) {
        m_deviceManager->ShutdownForProcessExit();
        m_deviceManager.reset();
    }
    m_notificationService.reset();
    m_trayController.reset();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnLaunched(
    [[maybe_unused]] LaunchActivatedEventArgs const& e) {
    if (!m_singleInstanceGuard.TryAcquire(L"AudioPlaybackConnector2_SingleInstance_v2")) {
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
    auto weak = get_weak();
    root.Loaded([weak, root](auto&, auto&) {
        if (auto self = weak.get()) {
            self->OnMainWindowLoaded(root);
        }
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
    m_deviceManager->StartDeviceWatcher();
    DebugTrace(L"[App] Device watcher started");
    bool willAutoReconnect = false;
    {
        auto locked = m_settings->LockSharedData();
        bool globalAutoReconnect = locked->GlobalAutoReconnect;
        for (const auto& id : locked->LastConnectedIds) {
            auto it = std::ranges::find_if(locked->Devices, [&](const auto& d) { return d.Id == id; });
            if (it != locked->Devices.end() && (globalAutoReconnect || it->AutoReconnect)) {
                willAutoReconnect = true;
                break;
            }
        }
    }

    if (m_notificationService && !willAutoReconnect) {
        m_notificationService->ShowAppStarted();
    }
    TryAutoReconnect();
    RefreshTrayVisualState(false);

    gdiplusGuard.release();

    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    m_trayController->UpdateTooltipFromConnections();
    CheckForUpdatesOnStartupAsync();
    DebugTrace(L"[App] Initialization complete");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Initializers //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::InitializeTray() {
    DebugTrace(L"[App] InitializeTray()");
    m_trayController = std::make_shared<TrayController>();
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
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->ConnectDetached(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->Disconnect(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.get(); self && self->m_deviceManager) self->m_deviceManager->ReconnectAsync(id);
        },
        [weak]() {
            if (auto self = weak.get()) self->ToggleLastConnectedDeviceFromTray();
        });
    m_trayController->PreloadDevicePicker();
    DebugTrace(L"[App] TrayController initialized");
}

void winrt::AudioPlaybackConnector2::implementation::App::InitializeNotifications() {
    DebugTrace(L"[App] InitializeNotifications()");
    m_notificationService = std::make_shared<NotificationService>();
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
    m_notificationService->SetFallbackNotifier([weak](std::wstring const& title,
                                                      std::wstring const& body,
                                                      NotificationService::FallbackNotificationType type) {
        if (auto self = weak.get()) {
            if (self->m_exiting.load() || !self->m_trayController) return;
            self->m_trayController->ShowNotification(title, body, ToTrayNotificationType(type));
        }
    });
    m_notificationsAvailable = m_notificationService->Initialize(
        winrt::hstring(_("AppName")), winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
    DebugTrace(L"[App] Notifications available: {0}", m_notificationsAvailable);
}

void winrt::AudioPlaybackConnector2::implementation::App::InitializeDeviceManager() {
    DebugTrace(L"[App] InitializeDeviceManager()");
    m_deviceManager = std::make_shared<DeviceManager>();
    m_settingsController = std::make_shared<SettingsController>(*m_settings, m_deviceManager);
    auto weak = get_weak();
    m_deviceManager->SetAutoReconnectPredicate([weak](auto id) {
        auto self = weak.get();
        if (!self || self->m_exiting.load() || !self->m_settings) return false;
        auto locked = self->m_settings->LockSharedData();
        if (locked->GlobalAutoReconnect) return true;
        return std::ranges::any_of(locked->Devices, [&](const auto& d) { return d.Id == id && d.AutoReconnect; });
    });
    DebugTrace(L"[App] DeviceManager initialized");
}

winrt::hstring
winrt::AudioPlaybackConnector2::implementation::App::ResolveKnownDeviceName(winrt::hstring const& id) const {
    if (!m_settings) return id;
    auto locked = m_settings->LockSharedData();
    auto it = std::ranges::find_if(locked->Devices, [&](const auto& device) { return device.Id == id; });
    if (it != locked->Devices.end()) return winrt::hstring(it->Name);
    return id;
}

void winrt::AudioPlaybackConnector2::implementation::App::ToggleLastConnectedDeviceFromTray() {
    if (m_exiting.load() || !m_settings || !m_deviceManager) return;

    std::wstring targetId;
    {
        auto locked = m_settings->LockSharedData();
        if (locked->LastConnectedIds.empty()) {
            DebugTrace(L"[App] Tray double-click ignored: no last connected device");
            return;
        }
        targetId = locked->LastConnectedIds.front();
    }

    if (targetId.empty()) return;

    auto id = winrt::hstring(targetId);

    if (m_deviceManager->IsDeviceBusy(id)) {
        DebugTrace(L"[App] Tray double-click ignored: device busy: {0}", targetId);
        return;
    }

    bool isConnected = false;
    for (const auto& c : m_deviceManager->GetConnectedDevices()) {
        if (c.Device.Id() == id) {
            isConnected = true;
            break;
        }
    }

    if (isConnected) {
        DebugTrace(L"[App] Tray double-click: disconnecting {0}", targetId);
        m_deviceManager->Disconnect(id);
    } else {
        DebugTrace(L"[App] Tray double-click: connecting {0}", targetId);
        m_deviceManager->ConnectDetached(id);
    }
    RefreshTrayVisualState(false);
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
        auto device = std::ranges::find_if(devices, [&](const auto& knownDevice) { return knownDevice.Id == id; });
        if (device != devices.end() && (globalAutoReconnect || device->AutoReconnect)) {
            DebugTrace(L"[App] Auto-reconnecting to: {0}", id);
            m_deviceManager->ConnectDetached(winrt::hstring(id));
        }
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::SaveLastConnectedDevices() {
    try {
        if (!m_settings || !m_deviceManager) return;

        auto connected = m_deviceManager->GetConnectedDevices();
        {
            auto locked = m_settings->LockExclusiveData();
            locked->LastConnectedIds.clear();
            for (const auto& c : connected) {
                try {
                    if (c.Device) {
                        locked->LastConnectedIds.push_back(std::wstring(c.Device.Id()));
                    }
                } catch (...) {
                }
            }
        }
        m_settings->Save(GetModuleHandleW(nullptr));
    } catch (...) {
        DebugTrace(L"[App] SaveLastConnectedDevices ERROR: ignored exception");
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::HandlePowerSuspend() {
    auto weak = get_weak();
    m_powerTransitionCoordinator.HandleSuspend(
        [weak]() {
            if (auto self = weak.get()) {
                self->SaveLastConnectedDevices();
            }
        },
        m_deviceManager);
}

void winrt::AudioPlaybackConnector2::implementation::App::HandlePowerResume() {
    auto weak = get_weak();
    m_powerTransitionCoordinator.HandleResume(m_deviceManager, [weak]() {
        if (auto self = weak.get()) {
            self->RunOnUIThread([weak]() {
                if (auto self = weak.get()) {
                    self->TryAutoReconnect();
                }
            });
        }
    });
}

winrt::fire_and_forget winrt::AudioPlaybackConnector2::implementation::App::CheckForUpdatesOnStartupAsync() {
    auto lifetime = get_strong();
    auto* settings = m_settings.get();
    auto notificationService = m_notificationService;
    if (m_exiting.load() || !settings || !notificationService) co_return;
    co_await StartupUpdateCoordinator::CheckForUpdatesAsync(*settings, notificationService, m_exiting);
}

void winrt::AudioPlaybackConnector2::implementation::App::RunOnUIThread(std::function<void()> work) {
    if (m_exiting.load()) return;
    if (!m_dispatcherQueue) return;
    if (m_dispatcherQueue.HasThreadAccess()) {
        if (m_exiting.load()) return;
        work();
    } else {
        auto weak = get_weak();
        m_dispatcherQueue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
                                     [weak, work = std::move(work)]() {
                                         auto self = weak.get();
                                         if (!self || self->m_exiting.load()) return;
                                         work();
                                     });
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::RefreshTrayVisualState(bool forceErrorWhenIdle) {
    if (m_exiting.load() || !m_trayController || !m_deviceManager) return;
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    const bool hasBusyOperations = m_deviceManager->HasBusyOperations();
    const bool hasConnections = m_deviceManager->HasConnections();

    if (forceErrorWhenIdle) {
        KillTimer(m_hwnd, c_timerAnimation);
        m_trayController->SetState(hasConnections ? TrayIconState::Connected : TrayIconState::Error);
    } else if (hasBusyOperations) {
        m_trayController->SetState(TrayIconState::Connecting);
        SetTimer(m_hwnd, c_timerAnimation, 200, nullptr);
    } else {
        KillTimer(m_hwnd, c_timerAnimation);
        m_trayController->SetState(hasConnections ? TrayIconState::Connected : TrayIconState::Idle);
    }

    if (!forceErrorWhenIdle) {
        m_trayController->UpdateTooltipFromConnections();
    }
    m_trayController->RefreshDevicePickerState();
}

void winrt::AudioPlaybackConnector2::implementation::App::SetupDeviceEvents() {
    DebugTrace(L"[App] SetupDeviceEvents()");
    auto weak = get_weak();
    DeviceEventRouter::Callbacks callbacks;
    callbacks.DeviceConnected = [weak](auto const& id) {
        if (auto self = weak.get()) self->OnDeviceConnected(id);
    };
    callbacks.DeviceDisconnected = [weak](auto const& id) {
        if (auto self = weak.get()) self->OnDeviceDisconnected(id);
    };
    callbacks.ConnectionError = [weak](auto const& id, auto const& msg) {
        if (auto self = weak.get()) self->OnConnectionError(id, msg);
    };
    callbacks.AutoReconnectTriggered = [weak](auto const& id) {
        if (auto self = weak.get()) self->OnAutoReconnectTriggered(id);
    };
    callbacks.AutoReconnectFailed = [weak](auto const& id) {
        if (auto self = weak.get()) self->OnAutoReconnectFailed(id);
    };
    callbacks.DeviceStatusChanged = [weak](auto const&, auto const& status) {
        auto self = weak.get();
        if (!self) return;
        if (self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
        if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
        bool forceErrorWhenIdle = !status.empty() && status != winrt::hstring(_("Connecting")) &&
                                  status != winrt::hstring(_("Reconnecting")) &&
                                  status != winrt::hstring(_("Connected"));
        self->RefreshTrayVisualState(forceErrorWhenIdle);
    };
    callbacks.DeviceActivityChanged = [weak]() {
        auto self = weak.get();
        if (!self || self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
        if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
        self->RefreshTrayVisualState(false);
    };

    m_deviceEventRouter.Attach(
        m_deviceManager,
        [weak](std::function<void()> work) {
            if (auto self = weak.get()) {
                self->RunOnUIThread(std::move(work));
            }
        },
        std::move(callbacks));
}

void winrt::AudioPlaybackConnector2::implementation::App::TeardownDeviceEvents() {
    m_deviceEventRouter.Detach();
}

void winrt::AudioPlaybackConnector2::implementation::App::ShowSettingsWindow() {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] ShowSettingsWindow()");
    auto weak = get_weak();
    m_settingsWindowPresenter.Show(m_settingsController, m_trayController, [weak]() {
        auto self = weak.get();
        if (!self || !self->m_settings) return;
        self->m_settings->Save(GetModuleHandleW(nullptr));
    });
}

void winrt::AudioPlaybackConnector2::implementation::App::ExitApplication() {
    if (m_exiting.exchange(true)) return;
    DebugTrace(L"[App] ExitApplication() started");

    m_powerTransitionCoordinator.Cancel();

    TeardownDeviceEvents();

    if (m_hwnd) {
        try {
            KillTimer(m_hwnd, c_timerAnimation);
            RemoveWindowSubclass(m_hwnd, SubclassProc, 1);
        } catch (...) {
        }
    }

    if (m_trayController) {
        m_trayController->Teardown();
    }

    if (m_notificationService) {
        m_notificationService->Teardown();
    }

    SaveLastConnectedDevices();

    if (m_deviceManager) {
        m_deviceManager->ShutdownForProcessExit();
    }
    m_notificationService.reset();

    m_trayController.reset();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    m_settingsWindowPresenter.Close();

    if (m_mainWindow) {
        m_mainWindow.Close();
    }
    DebugTrace(L"[App] ExitApplication() complete");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Handlers /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    bool updatedLastConnected = false;
    {
        auto locked = m_settings->LockExclusiveData();
        bool alreadyKnown = std::ranges::any_of(locked->Devices, [&](const auto& d) { return d.Id == id; });
        if (!alreadyKnown) {
            DeviceSettings newDevice;
            newDevice.Id = std::wstring(id);
            newDevice.Name = std::wstring(deviceName);
            newDevice.AutoReconnect = locked->GlobalAutoReconnect;
            locked->Devices.push_back(std::move(newDevice));
            addedNew = true;
        }

        auto existing = std::ranges::find(locked->LastConnectedIds, std::wstring(id));
        if (existing != locked->LastConnectedIds.end()) {
            locked->LastConnectedIds.erase(existing);
        }
        locked->LastConnectedIds.insert(locked->LastConnectedIds.begin(), std::wstring(id));
        updatedLastConnected = true;
    }

    if (addedNew || updatedLastConnected) {
        m_settings->Save(GetModuleHandleW(nullptr));
        if (addedNew) {
            DebugTrace(L"[App] New device added to settings: {0}", std::wstring(deviceName));
        }
    }

    {
        auto locked = m_settings->LockSharedData();
        bool autoReconnect = locked->GlobalAutoReconnect;
        auto it = std::ranges::find_if(locked->Devices, [&](const auto& d) { return d.Id == id; });
        if (it != locked->Devices.end()) autoReconnect = autoReconnect || it->AutoReconnect;
        m_deviceManager->SetAutoReconnect(id, autoReconnect);
    }

    m_notificationService->ShowDeviceConnected(id, deviceName);

    RefreshTrayVisualState(false);
}

void winrt::AudioPlaybackConnector2::implementation::App::OnDeviceDisconnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_settings || !m_notificationService || !m_trayController) return;
    DebugTrace(L"[App] OnDeviceDisconnected: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    m_notificationService->ShowDeviceDisconnected(id, deviceName);

    RefreshTrayVisualState(false);
}

void winrt::AudioPlaybackConnector2::implementation::App::OnConnectionError(winrt::hstring const& id,
                                                                            winrt::hstring msg) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnConnectionError: {0} - {1}", std::wstring(id), std::wstring(msg));
    if (m_trayController) {
        std::wstring tip = std::wstring(_("AppName")) + L"\n" + std::wstring(msg);
        m_trayController->UpdateTooltip(tip);
    }
    RefreshTrayVisualState(true);
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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Window Subclass ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

LRESULT CALLBACK winrt::AudioPlaybackConnector2::implementation::App::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) {
    auto* app = reinterpret_cast<App*>(dwRefData);
    if (!app) return DefSubclassProc(hwnd, msg, wParam, lParam);
    if (app->m_exiting.load()) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (app->m_trayController && msg == app->m_trayController->TrayCallbackMessage()) {
        app->m_trayController->HandleTrayMessage(wParam, lParam);
        return 0;
    }

    if (msg == WM_SETTINGCHANGE) {
        ThemeHelper::OnSettingChange(hwnd, lParam);
        return 0;
    }

    if (msg == WM_POWERBROADCAST) {
        switch (wParam) {
            case PBT_APMSUSPEND: app->HandlePowerSuspend(); return TRUE;
            case PBT_APMRESUMEAUTOMATIC:
            case PBT_APMRESUMESUSPEND: app->HandlePowerResume(); return TRUE;
            default: break;
        }
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
