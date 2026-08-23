#include <pch.h>

#include <app/ApplicationHost.hpp>

#include <MainWindow/MainWindow.xaml.h>
#include <app/AutoReconnectPlanner.hpp>
#include <app/StartupUpdateCoordinator.hpp>
#include <core/DeviceService.hpp>
#include <core/SettingsLimits.hpp>
#include <core/SettingsStore.hpp>
#include <core/StringResources.hpp>
#include <core/ThemeHelper.hpp>
#include <services/UpdateCoordinator.hpp>
#include <services/UpdateService.hpp>
#include <ui/TrayContextMenu.hpp>
#include <ui/TrayIcon.hpp>
#include <ui/XamlWindowInterop.hpp>
#include <util/CrashHandler.hpp>
#include <util/Logger.hpp>
#include <util/Util.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

constexpr DWORD c_controlWaitPollMs = 50;
constexpr int c_hiddenAnchorCoordinate = -32000;
constexpr auto c_resourcePressureSnapshotMaximumAge = std::chrono::seconds{75};
constexpr auto c_mainWindowLoadedTimeout = std::chrono::seconds{15};

using Bridge = apc::app::LegacyAppUseCaseBridge;
using OperationStatus = Bridge::OperationStatus;

OperationStatus ToUiActionStatus(ControlUiActionGate::Result result) noexcept {
    switch (result) {
        case ControlUiActionGate::Result::Succeeded: return OperationStatus::Succeeded;
        case ControlUiActionGate::Result::Failed: return OperationStatus::Failed;
        case ControlUiActionGate::Result::Indeterminate: return OperationStatus::Indeterminate;
    }
    return OperationStatus::Failed;
}

apc::app::DeviceConnectionState ToAppDeviceState(DeviceStatusKind status) noexcept {
    switch (status) {
        case DeviceStatusKind::Ready:
        case DeviceStatusKind::None: return apc::app::DeviceConnectionState::Idle;
        case DeviceStatusKind::Connecting: return apc::app::DeviceConnectionState::Connecting;
        case DeviceStatusKind::Connected: return apc::app::DeviceConnectionState::Connected;
        case DeviceStatusKind::Reconnecting: return apc::app::DeviceConnectionState::WaitingForReconnect;
        case DeviceStatusKind::Error: return apc::app::DeviceConnectionState::Failed;
    }
    return apc::app::DeviceConnectionState::Idle;
}

apc::app::AppSnapshot::ResourceStatusSnapshot::Residency ToAppResidency(ResidencyPolicy value) noexcept {
    using Residency = apc::app::AppSnapshot::ResourceStatusSnapshot::Residency;
    switch (value) {
        case ResidencyPolicy::Cold: return Residency::Cold;
        case ResidencyPolicy::Warm: return Residency::Warm;
        case ResidencyPolicy::Hot: return Residency::Hot;
    }
    return Residency::Warm;
}

apc::app::AppSnapshot::ResourceStatusSnapshot::MemoryPressure ToAppMemoryPressure(MemoryPressureState value) noexcept {
    using MemoryPressure = apc::app::AppSnapshot::ResourceStatusSnapshot::MemoryPressure;
    switch (value) {
        case MemoryPressureState::Unknown: return MemoryPressure::Unknown;
        case MemoryPressureState::Low: return MemoryPressure::Low;
        case MemoryPressureState::Neutral: return MemoryPressure::Neutral;
        case MemoryPressureState::High: return MemoryPressure::High;
    }
    return MemoryPressure::Unknown;
}

apc::app::AppSnapshot::ResourceStatusSnapshot::UserActivity ToAppUserActivity(UserActivityState value) noexcept {
    using UserActivity = apc::app::AppSnapshot::ResourceStatusSnapshot::UserActivity;
    switch (value) {
        case UserActivityState::Unknown: return UserActivity::Unknown;
        case UserActivityState::Available: return UserActivity::Available;
        case UserActivityState::NotPresent: return UserActivity::NotPresent;
        case UserActivityState::Busy: return UserActivity::Busy;
        case UserActivityState::Fullscreen: return UserActivity::Fullscreen;
        case UserActivityState::Presentation: return UserActivity::Presentation;
        case UserActivityState::QuietTime: return UserActivity::QuietTime;
        case UserActivityState::ImmersiveApp: return UserActivity::ImmersiveApp;
    }
    return UserActivity::Unknown;
}

template <typename TAsync>
OperationStatus WaitForControlAsync(TAsync const& operation, apc::app::AppCommandContext const& context) {
    std::shared_ptr<void> completed(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* handle) noexcept {
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
    });
    if (!completed) return OperationStatus::Failed;

    operation.Completed(
        [completed](auto const&, auto const&) noexcept { SetEvent(static_cast<HANDLE>(completed.get())); });
    while (true) {
        if (context.IsCancellationRequested()) {
            operation.Cancel();
            return OperationStatus::Cancelled;
        }

        DWORD remaining = INFINITE;
        if (context.Deadline != apc::app::AppCommandContext::TimePoint::max()) {
            const auto duration = context.Deadline - apc::app::AppCommandContext::Clock::now();
            if (duration <= std::chrono::steady_clock::duration::zero()) {
                operation.Cancel();
                return OperationStatus::TimedOut;
            }
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            remaining = static_cast<DWORD>(std::clamp<std::int64_t>(milliseconds, 1, INFINITE));
        }
        if (remaining == 0) {
            operation.Cancel();
            return OperationStatus::TimedOut;
        }
        const auto waitResult =
            WaitForSingleObject(static_cast<HANDLE>(completed.get()), std::min<DWORD>(remaining, c_controlWaitPollMs));
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_TIMEOUT) {
            operation.Cancel();
            return OperationStatus::Failed;
        }
    }

    switch (operation.Status()) {
        case winrt::Windows::Foundation::AsyncStatus::Completed: return OperationStatus::Succeeded;
        case winrt::Windows::Foundation::AsyncStatus::Canceled: return OperationStatus::Cancelled;
        default: return OperationStatus::Failed;
    }
}

void LogMainWindowAnchor(HWND hwnd, std::wstring_view reason) noexcept {
    if (!hwnd) return;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    auto const style = GetWindowLongPtr(hwnd, GWL_STYLE);
    auto const exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    DebugTrace(L"[App] MainWindow anchor reason={0} hwnd=0x{1:X} visible={2} rect=({3},{4})-({5},{6}) "
               L"size={7}x{8} style=0x{9:08X} exStyle=0x{10:08X}",
               reason,
               reinterpret_cast<uintptr_t>(hwnd),
               IsWindowVisible(hwnd) != FALSE,
               rect.left,
               rect.top,
               rect.right,
               rect.bottom,
               rect.right - rect.left,
               rect.bottom - rect.top,
               static_cast<uint32_t>(style),
               static_cast<uint32_t>(exStyle));
}

void ConfigureHiddenMainWindowAnchor(HWND hwnd) noexcept {
    if (!hwnd) return;

    auto const oldStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
    auto const newStyle =
        (oldStyle & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) |
        static_cast<LONG_PTR>(WS_POPUP);
    SetWindowLongPtr(hwnd, GWL_STYLE, newStyle);

    auto const oldExStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    auto const newExStyle = oldExStyle | static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, newExStyle);

    SetWindowPos(hwnd,
                 HWND_BOTTOM,
                 c_hiddenAnchorCoordinate,
                 c_hiddenAnchorCoordinate,
                 1,
                 1,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    LogMainWindowAnchor(hwnd, L"configured-hidden-anchor");
}

[[noreturn]] void TerminateAfterWindowCloseFailure(std::wstring_view reason) noexcept {
    DebugTrace(L"[App] FATAL: window teardown failed reason={0}; terminating process", reason);
    util::FlushInMemoryLogTailToFile(reason, ERROR_PROCESS_ABORTED);
    ExitProcess(ERROR_PROCESS_ABORTED);
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

ApplicationHost::ApplicationHost() {
    util::crash::InstallCrashHandlers();
}

ApplicationHost::~ApplicationHost() {
    Shutdown();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::Start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        DebugTrace(L"[App] Start ignored because initialization already began");
        return;
    }
    if (!m_singleInstanceGuard.TryAcquire(L"AudioPlaybackConnector2_SingleInstance_v2")) {
        ExitProcess(0);
        return;
    }
    DebugTrace(L"[App] OnLaunched started");
    try {
        SetupMainWindow();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Main window setup failed", ex);
        FailStartup(L"main-window-setup-hresult");
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Main window setup failed", ex);
        FailStartup(L"main-window-setup-standard");
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Main window setup failed");
        FailStartup(L"main-window-setup-unknown");
    }
}

bool ApplicationHost::PerformTeardown(SettingsShutdownMode settingsShutdownMode) noexcept {
    if (m_exiting.exchange(true)) return m_teardownWindowCloseSucceeded.load();
    // Request pipe-handler cancellation before waiting for bridge leases. A
    // handler may be waiting for this UI thread to process show/settings work;
    // Stop() still drains the server after the bridge has rejected new work.
    m_commandLineControlServer.RequestStop();
    if (m_appBridge) {
        m_appBridge->SetRunning(false);
    }

    StopMainWindowLoadedWatchdog();
    if (auto notification = std::exchange(m_powerSavingStatusNotification, nullptr)) {
        if (!UnregisterPowerSettingNotification(notification)) {
            DebugTrace(L"[App] Failed to unregister battery-saver notification: {0}", GetLastError());
        }
    }
    if (m_resourcePressureMonitor) {
        m_resourcePressureMonitor->Stop();
        m_resourcePressureMonitor.reset();
    }
    if (m_adaptiveResourceFallbackTimer) {
        try {
            m_adaptiveResourceFallbackTimer.Stop();
        } catch (...) {
        }
        m_adaptiveResourceFallbackTimer = nullptr;
    }
    static_cast<void>(m_adaptiveScheduleState.Supersede());
    m_deviceVisualRefreshCoalescer.Cancel();
    CancelNativeDeviceVisualRefreshRetry();
    {
        std::scoped_lock lock(m_uiFallbackWorkMutex);
        m_uiFallbackWork.clear();
        m_uiFallbackMessagePending = false;
    }

    if (m_mainWindowLoadedToken.value != 0 && m_mainWindow) {
        try {
            if (auto root = m_mainWindow.Content().try_as<Controls::Grid>()) {
                root.Loaded(m_mainWindowLoadedToken);
            }
        } catch (...) {
        }
        m_mainWindowLoadedToken = {};
    }
    m_powerTransitionCoordinator.Cancel();
    // Close the settings window while its controller and Store are still alive so
    // the final placement mutation can be committed before Store shutdown.
    auto const settingsWindowClosed = m_settingsWindowPresenter.Close();
    m_teardownWindowCloseSucceeded.store(settingsWindowClosed);
    if (m_updateCoordinator) {
        m_updateCoordinator->Shutdown();
    }
    if (m_startupTaskCoordinator) {
        m_startupTaskCoordinator->Shutdown();
    }
    m_commandLineControlServer.Stop();
    m_controlCommandAdapter.reset();
    TeardownDeviceEvents();
    m_appController.reset();
    m_appBridge.reset();
    if (m_hwnd) {
        try {
            KillTimer(m_hwnd, c_timerAnimation);
            KillTimer(m_hwnd, c_timerTransientTrayError);
            KillTimer(m_hwnd, c_timerAdaptiveResources);
            KillTimer(m_hwnd, c_timerDeviceVisualRefreshRetry);
            m_connectingAnimationTimerActive = false;
            if (m_windowSubclassInstalled) {
                if (RemoveWindowSubclass(m_hwnd, SubclassProc, 1)) {
                    m_windowSubclassInstalled = false;
                } else {
                    DebugTrace(L"[App] ERROR: failed to remove MainWindow subclass: {0}", GetLastError());
                }
            }
        } catch (...) {
        }
    }
    if (m_trayController) {
        m_trayController->Teardown();
    }
    if (m_notificationService) {
        m_notificationService->Teardown();
    }
    if (m_deviceService) {
        m_deviceService->ShutdownForProcessExit();
        m_deviceService.reset();
    }
    m_notificationService.reset();
    m_updateCoordinator.reset();
    m_startupTaskCoordinator.reset();
    m_trayController.reset();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
    m_settingsController.reset();
    if (m_settingsStore) {
        const auto settingsShutdown = m_settingsStore->Shutdown(settingsShutdownMode, 3);
        if (!settingsShutdown) {
            DebugTrace(L"[App] SettingsStore shutdown failed mode={0}",
                       settingsShutdownMode == SettingsShutdownMode::Flush ? L"flush" : L"discard-startup-failure");
        }
        m_settingsStore.reset();
    }
    return settingsWindowClosed;
}
void ApplicationHost::Shutdown() noexcept {
    static_cast<void>(PerformTeardown(SettingsShutdownMode::Flush));
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::SetupMainWindow() {
    m_mainWindow = winrt::make<winrt::AudioPlaybackConnector2::implementation::MainWindow>();
    m_mainWindow.Title(winrt::hstring(L"AudioPlaybackConnector2"));
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
        FailStartup(L"main-window-content-null");
        return;
    }

    auto root = content.try_as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[App] ERROR: MainWindow.Content() is not a Grid!");
        FailStartup(L"main-window-content-type");
        return;
    }

    // Hide the Grid until the window is positioned off-screen to avoid
    // a visible black/white flash during startup.
    root.Opacity(0);

    // Use Grid.Loaded instead of Window.Activated.
    // Loaded fires after the element is added to the visual tree and XamlRoot
    // has been assigned, which is required for MenuFlyout anchoring.
    auto weak = weak_from_this();
    m_mainWindowLoadedToken = root.Loaded([weak, root](auto&, auto&) noexcept {
        try {
            if (auto self = weak.lock()) {
                if (self->m_mainWindowLoadedToken.value != 0) {
                    root.Loaded(self->m_mainWindowLoadedToken);
                    self->m_mainWindowLoadedToken = {};
                }
                self->OnMainWindowLoaded(root);
            }
        } catch (...) {
            if (auto self = weak.lock()) self->FailStartup(L"main-window-loaded-callback");
        }
    });

    StartMainWindowLoadedWatchdog();
    m_mainWindow.Activate();
    DebugTrace(L"[App] MainWindow.Activate() called");
}

void ApplicationHost::StartMainWindowLoadedWatchdog() {
    if (!m_dispatcherQueue || m_hwnd || m_exiting.load()) return;

    auto timer = m_dispatcherQueue.CreateTimer();
    timer.Interval(c_mainWindowLoadedTimeout);
    timer.IsRepeating(false);
    auto weak = weak_from_this();
    timer.Tick([weak](auto const&, auto const&) noexcept {
        try {
            if (auto self = weak.lock(); self && !self->m_exiting.load() && !self->m_hwnd) {
                self->FailStartup(L"main-window-loaded-timeout");
            }
        } catch (...) {
            if (auto self = weak.lock()) self->FailStartup(L"main-window-loaded-watchdog");
        }
    });
    m_mainWindowLoadedWatchdog = timer;
    timer.Start();
}

void ApplicationHost::StopMainWindowLoadedWatchdog() noexcept {
    auto timer = std::exchange(m_mainWindowLoadedWatchdog, nullptr);
    if (!timer) return;
    try {
        timer.Stop();
    } catch (...) {
    }
}

void ApplicationHost::OnMainWindowLoaded(Controls::Grid const& root) noexcept try {
    if (m_hwnd) {
        DebugTrace(L"[App] Grid.Loaded fired again, ignoring (already initialized)");
        return;
    }
    StopMainWindowLoadedWatchdog();
    DebugTrace(L"[App] Grid.Loaded - beginning initialization");

    m_hwnd = util::GetWindowHandle(m_mainWindow);
    if (!m_hwnd) {
        DebugTrace(L"[App] ERROR: GetWindowHandle returned null!");
        FailStartup(L"main-window-hwnd-null");
        return;
    }
    DebugTrace(L"[App] MainWindow HWND = 0x{0:X}", reinterpret_cast<uintptr_t>(m_hwnd));

    ConfigureHiddenMainWindowAnchor(m_hwnd);
    root.Opacity(1);
    DebugTrace(L"[App] MainWindow hidden anchor configured");

    if (!SetWindowSubclass(m_hwnd, SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
        DebugTrace(L"[App] ERROR: SetWindowSubclass failed: {0}", GetLastError());
        FailStartup(L"main-window-subclass");
        return;
    }
    m_windowSubclassInstalled = true;
    DebugTrace(L"[App] Window subclass installed");

    m_settingsStore = std::make_shared<SettingsStore>();
    m_settingsStore->Load();
    DebugTrace(L"[App] Settings loaded");

    const auto settingsSnapshot = m_settingsStore->Snapshot();
    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), settingsSnapshot.Data.Language);
    DebugTrace(L"[App] StringResources initialized");

    m_updateCoordinator = std::make_shared<UpdateCoordinator>(
        [](std::stop_token stopToken) { return UpdateService::CheckForUpdatesAsync(stopToken); });
    DebugTrace(L"[App] UpdateCoordinator initialized");

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        DebugTrace(L"[App] ERROR: GdiplusStartup failed");
        FailStartup(L"gdiplus-startup");
        return;
    }
    DebugTrace(L"[App] GDI+ initialized");

    InitializeDeviceService();
    InitializeAppController();
    InitializeTray();
    InitializeAdaptiveResources();
    InitializeNotifications();
    SetupDeviceEvents();
    const auto incomingSettingsSnapshot = m_settingsStore->Snapshot();
    m_deviceService->ConfigureIncomingConnections(incomingSettingsSnapshot.Data.AllowIncomingConnections);
    std::vector<std::wstring> individuallyEnabledReconnectIds;
    for (auto const& device : incomingSettingsSnapshot.Data.Devices) {
        if (device.ReconnectOnConnectionLoss) individuallyEnabledReconnectIds.push_back(device.Id);
    }
    m_deviceService->ConfigureReconnectPolicy(incomingSettingsSnapshot.Data.GlobalReconnectOnConnectionLoss,
                                              individuallyEnabledReconnectIds);
    static_cast<void>(m_deviceService->Start());
    DebugTrace(L"[App] Device watcher started");
    InitializeCommandLineControl();
    const auto reconnectSettingsSnapshot = m_settingsStore->Snapshot();
    const bool willAutoReconnect = AutoReconnectPlanner::HasReconnectTargets(reconnectSettingsSnapshot.Data);

    if (m_notificationService && !willAutoReconnect) {
        try {
            m_notificationService->ShowAppStarted();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] startup notification failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] startup notification failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] startup notification failed");
        }
    }
    TryAutoReconnect();
    ScheduleDeviceVisualRefresh(false);

    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    CheckForUpdatesOnStartupAsync();
    util::crash::CheckAndPromptCrashReports();
    DebugTrace(L"[App] Initialization complete");
} catch (winrt::hresult_error const& ex) {
    util::DebugTraceException(L"[App] Initialization after MainWindow.Loaded failed", ex);
    FailStartup(L"main-window-loaded-hresult");
} catch (std::exception const& ex) {
    util::DebugTraceException(L"[App] Initialization after MainWindow.Loaded failed", ex);
    FailStartup(L"main-window-loaded-standard");
} catch (...) {
    util::DebugTraceUnknownException(L"[App] Initialization after MainWindow.Loaded failed");
    FailStartup(L"main-window-loaded-unknown");
}

void ApplicationHost::FailStartup(std::wstring_view stage) noexcept {
    DebugTrace(L"[App] Startup aborted at stage={0}", stage);
    auto const settingsWindowClosed = PerformTeardown(SettingsShutdownMode::DiscardStartupFailure);
    auto const mainWindowClosed = CloseMainWindow(L"startup-failure");
    if (!settingsWindowClosed || !mainWindowClosed) TerminateAfterWindowCloseFailure(L"startup-window-close-failure");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Initializers //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::InitializeTray() {
    DebugTrace(L"[App] InitializeTray()");
    m_trayController = std::make_shared<TrayController>();
    m_trayController->Initialize(m_hwnd, m_mainWindow);
    m_trayController->SetDeviceService(m_deviceService);
    m_trayController->SetSettingsStore(m_settingsStore);
    auto weak = weak_from_this();
    if (m_settingsController) {
        m_settingsController->SetPresentationChangedCallback([weak](ISettingsController::PresentationChangeKind kind) {
            auto self = weak.lock();
            if (!self || self->m_exiting.load()) return;
            if (kind == ISettingsController::PresentationChangeKind::Language && self->m_trayController) {
                self->m_trayController->ApplyLanguage();
                return;
            }
            if (kind == ISettingsController::PresentationChangeKind::Appearance && self->m_trayController &&
                self->m_settingsStore) {
                const auto settingsSnapshot = self->m_settingsStore->Snapshot();
                self->m_trayController->SetSystemBackdropEffectsEnabled(settingsSnapshot.Data.UseSystemBackdropEffects);
                return;
            }
            self->ScheduleDeviceVisualRefresh(false);
        });
    }
    m_trayController->SetCallbacks(
        [weak]() {
            if (auto self = weak.lock()) {
                self->ExecuteTrayCommand(apc::app::AppCommand{apc::app::AppCommandKind::ShowSettings, {}, {}});
            }
        },
        apc::ui::MakeTrayPrimaryActivationCallback(
            [weak](apc::app::AppCommand command, apc::app::AppCommandContext context) {
                if (auto self = weak.lock()) {
                    self->ExecuteTrayCommand(std::move(command), context.Completion);
                }
            }),
        [weak]() {
            if (auto self = weak.lock()) self->ExitApplication();
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock()) {
                if (auto selector = apc::app::DeviceSelector::ById(std::wstring(id))) {
                    self->ExecuteTrayCommand(
                        apc::app::AppCommand{apc::app::AppCommandKind::Connect, std::move(*selector), {}});
                }
            }
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock()) {
                if (auto selector = apc::app::DeviceSelector::ById(std::wstring(id))) {
                    self->ExecuteTrayCommand(
                        apc::app::AppCommand{apc::app::AppCommandKind::Disconnect, std::move(*selector), {}});
                }
            }
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock()) {
                if (auto selector = apc::app::DeviceSelector::ById(std::wstring(id))) {
                    self->ExecuteTrayCommand(
                        apc::app::AppCommand{apc::app::AppCommandKind::Reconnect, std::move(*selector), {}});
                }
            }
        },
        [weak]() {
            if (auto self = weak.lock()) {
                self->ExecuteTrayCommand(apc::app::AppCommand{
                    apc::app::AppCommandKind::ToggleLast, apc::app::DeviceSelector::Default(), {}});
            }
        },
        [weak]() {
            if (auto self = weak.lock()) {
                self->ExecuteTrayCommand(apc::app::AppCommand{apc::app::AppCommandKind::DisconnectAll, {}, {}});
            }
        },
        [weak]() {
            if (auto self = weak.lock()) {
                self->ExecuteTrayCommand(apc::app::AppCommand{apc::app::AppCommandKind::ReconnectAll, {}, {}});
            }
        });
    DebugTrace(L"[App] TrayController initialized");
}

void ApplicationHost::InitializeNotifications() {
    DebugTrace(L"[App] InitializeNotifications()");
    m_notificationService = std::make_shared<NotificationService>();
    auto weak = weak_from_this();
    m_notificationService->SetShouldShowNotificationCallback([weak]() -> bool {
        if (auto self = weak.lock()) {
            if (self->m_settingsStore) return self->m_settingsStore->Snapshot().Data.ShowNotifications;
        }
        return true;
    });
    m_notificationService->SetReconnectCallback([weak](winrt::hstring deviceId) {
        if (auto self = weak.lock()) {
            static_cast<void>(self->RunOnUIThread([weak, deviceId = std::move(deviceId)]() mutable {
                if (auto self = weak.lock()) {
                    if (self->m_exiting.load()) return;
                    if (auto selector = apc::app::DeviceSelector::ById(std::wstring(deviceId))) {
                        self->ExecuteTrayCommand(
                            apc::app::AppCommand{apc::app::AppCommandKind::Reconnect, std::move(*selector), {}});
                    }
                }
            }));
        }
    });
    const auto notificationsAvailable = m_notificationService->Initialize(
        winrt::hstring(_("AppName")), winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
    DebugTrace(L"[App] Notifications available: {0}", notificationsAvailable);
}

void ApplicationHost::InitializeDeviceService() {
    DebugTrace(L"[App] InitializeDeviceService()");
    m_deviceService = std::make_shared<apc::device::DeviceService>();
    auto weak = weak_from_this();
    m_settingsController = std::make_shared<SettingsController>(m_settingsStore, m_deviceService);
    auto weakSettingsController = std::weak_ptr<ISettingsController>(m_settingsController);
    m_startupTaskCoordinator = std::make_shared<StartupTaskCoordinator>([weakSettingsController](bool enabled) {
        if (auto controller = weakSettingsController.lock()) controller->SetStartWithWindows(enabled);
    });
    DebugTrace(L"[App] DeviceService initialized");
}

void ApplicationHost::InitializeAppController() {
    DebugTrace(L"[App] InitializeAppController()");
    auto weak = weak_from_this();
    Bridge::Operations operations;
    operations.ReadSettings = [weak]() {
        if (auto self = weak.lock(); self && self->m_settingsStore) return self->m_settingsStore->Snapshot();
        throw std::runtime_error("ApplicationHost settings store unavailable");
    };
    operations.ReadConnectedDevices = [weak]() {
        std::vector<Bridge::DeviceRecord> devices;
        auto self = weak.lock();
        if (!self || !self->m_deviceService) return devices;

        for (auto const& connection : self->m_deviceService->GetConnectionSessions()) {
            if (connection.DeviceId.empty()) continue;
            const auto id = winrt::hstring(connection.DeviceId);
            const auto isBusy = self->m_deviceService->IsDeviceBusy(id);
            const auto state = [&] {
                switch (connection.State) {
                    case apc::device::DeviceLifecycleState::Connected:
                        return apc::app::DeviceConnectionState::Connected;
                    case apc::device::DeviceLifecycleState::WaitingForReconnect:
                        return apc::app::DeviceConnectionState::WaitingForReconnect;
                    case apc::device::DeviceLifecycleState::Failed: return apc::app::DeviceConnectionState::Failed;
                    case apc::device::DeviceLifecycleState::Connecting:
                    case apc::device::DeviceLifecycleState::Disconnecting:
                        return apc::app::DeviceConnectionState::Connecting;
                    case apc::device::DeviceLifecycleState::Idle: return apc::app::DeviceConnectionState::Idle;
                }
                return apc::app::DeviceConnectionState::Idle;
            }();
            devices.push_back({.Id = connection.DeviceId,
                               .Name = connection.DeviceName,
                               .Alias = {},
                               .State = state,
                               .IsConnected = state == apc::app::DeviceConnectionState::Connected,
                               .IsKnown = true,
                               .IsBusy = isBusy});
        }
        return devices;
    };
    operations.Refresh = [weak](apc::app::AppCommandContext const& context) {
        Bridge::RefreshResult result;
        auto self = weak.lock();
        if (!self || !self->m_deviceService) return result;

        try {
            auto operation = self->m_deviceService->RefreshDevicesAsync();
            result.Status = WaitForControlAsync(operation, context);
            if (result.Status != OperationStatus::Succeeded) return result;

            auto devices = operation.GetResults();
            if (!devices) return result;
            result.Devices.reserve(devices.Size());
            for (auto const& device : devices) {
                const auto id = std::wstring(device.Id());
                if (id.empty()) continue;
                result.Devices.push_back({.Id = id,
                                          .Name = std::wstring(device.Name()),
                                          .Alias = {},
                                          .State = apc::app::DeviceConnectionState::Idle,
                                          .IsConnected = false,
                                          .IsKnown = false,
                                          .IsBusy = self->m_deviceService->IsDeviceBusy(winrt::hstring(id))});
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Control command device refresh failed", ex);
            result.Status = OperationStatus::Failed;
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Control command device refresh failed", ex);
            result.Status = OperationStatus::Failed;
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Control command device refresh failed");
            result.Status = OperationStatus::Failed;
        }
        return result;
    };
    operations.Connect = [weak](std::wstring_view deviceId, apc::app::AppCommandContext const& context) {
        Bridge::OperationResult result;
        auto self = weak.lock();
        if (!self || !self->m_deviceService) return result;
        try {
            result.Status = WaitForControlAsync(self->m_deviceService->ConnectAsync(winrt::hstring(deviceId)), context);
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Control command connect failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Control command connect failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Control command connect failed");
        }
        return result;
    };
    operations.ConnectDetached = [weak](std::wstring_view deviceId) {
        if (auto self = weak.lock(); self && self->m_deviceService && !self->m_exiting.load()) {
            self->m_deviceService->ConnectDetached(winrt::hstring(deviceId));
        }
    };
    operations.Reconnect = [weak](std::wstring_view deviceId, apc::app::AppCommandContext const& context) {
        Bridge::OperationResult result;
        auto self = weak.lock();
        if (!self || !self->m_deviceService) return result;
        try {
            result.Status =
                WaitForControlAsync(self->m_deviceService->ReconnectAsync(winrt::hstring(deviceId)), context);
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Control command reconnect failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Control command reconnect failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Control command reconnect failed");
        }
        return result;
    };
    operations.ReconnectDetached = [weak](std::wstring_view deviceId) {
        if (auto self = weak.lock(); self && self->m_deviceService && !self->m_exiting.load()) {
            self->m_deviceService->ReconnectDetached(winrt::hstring(deviceId));
        }
    };
    operations.Disconnect = [weak](std::wstring_view deviceId) {
        if (auto self = weak.lock(); self && self->m_deviceService && !self->m_exiting.load()) {
            static_cast<void>(self->m_deviceService->Disconnect(std::wstring(deviceId)));
        }
    };
    operations.DisconnectAll = [weak]() {
        if (auto self = weak.lock(); self && self->m_deviceService && !self->m_exiting.load()) {
            static_cast<void>(self->m_deviceService->DisconnectAll());
        }
    };
    operations.ReconnectAllDetached = [weak]() {
        if (auto self = weak.lock(); self && self->m_deviceService && !self->m_exiting.load()) {
            static_cast<void>(self->m_deviceService->ReconnectAll());
        }
    };
    operations.SetDefaultDevice = [weak](std::wstring_view deviceId) {
        if (auto self = weak.lock(); self && self->m_settingsController) {
            return self->m_settingsController->SetDefaultDeviceId(std::wstring(deviceId));
        }
        return false;
    };
    operations.ClearDefaultDevice = [weak]() {
        if (auto self = weak.lock(); self && self->m_settingsController) {
            return self->m_settingsController->ClearDefaultDevice();
        }
        return false;
    };
    operations.SetDeviceAlias =
        [weak](std::wstring_view deviceId, std::wstring_view alias, std::wstring_view deviceName) {
            if (auto self = weak.lock(); self && self->m_settingsController) {
                return self->m_settingsController->SetDeviceAlias(
                    std::wstring(deviceId), std::wstring(alias), std::wstring(deviceName));
            }
            // Preserve the legacy missing-controller success behavior.
            return true;
        };
    operations.ShowDevicePicker = [weak](apc::app::DevicePickerOpenMode openMode,
                                         apc::app::AppCommandContext const& context) {
        Bridge::UiActionResult result;
        auto self = weak.lock();
        if (!self || !self->m_trayController) return result;

        auto tray = self->m_trayController;
        const auto openedGeneration = tray->DevicePickerOpenedGeneration();
        const auto wasVisible = tray->IsDevicePickerVisibleOrTransitioning();
        const auto uiResult = self->RunControlUiAction(
            [weak, openMode]() {
                auto self = weak.lock();
                return self && self->m_trayController &&
                       self->m_trayController->ShowDevicePicker(openMode ==
                                                                apc::app::DevicePickerOpenMode::ToggleIfOpen);
            },
            context);
        if (uiResult != ControlUiActionResult::Succeeded) {
            // The gate records whether the dispatcher crossed TryBegin. A
            // canceled or expired context alone cannot distinguish an action
            // that never ran from one that may have already mutated the UI.
            result.Status = ToUiActionStatus(uiResult);
            return result;
        }

        // Tray activation is dispatched detached from the UI callback.  The
        // flyout's Opened event is posted back to this same dispatcher, so a
        // detached UI-thread command must not wait for its generation here.
        // Control `show` remains WaitForCompletion and keeps the existing
        // acknowledgement semantics below.
        if (context.Completion == apc::app::AppCommandContext::CompletionMode::Detached) {
            result.Status = OperationStatus::Succeeded;
            result.DevicePickerOpenedGeneration = tray->DevicePickerOpenedGeneration();
            return result;
        }

        while (!wasVisible && tray->DevicePickerOpenedGeneration() == openedGeneration) {
            if (context.IsCancellationRequested()) {
                // ShowDevicePicker has already begun. The caller cannot know
                // whether the UI transition will publish its generation after
                // this return, so a definite cancellation would be unsafe.
                result.Status = OperationStatus::Indeterminate;
                return result;
            }
            if (context.IsExpired(apc::app::AppCommandContext::Clock::now())) {
                result.Status = OperationStatus::Indeterminate;
                return result;
            }
            Sleep(1);
        }
        result.Status = OperationStatus::Succeeded;
        result.DevicePickerOpenedGeneration = tray->DevicePickerOpenedGeneration();
        return result;
    };
    operations.ShowSettings = [weak](apc::app::AppCommandContext const& context) {
        Bridge::UiActionResult result;
        auto self = weak.lock();
        if (!self) return result;
        const auto uiResult = self->RunControlUiAction(
            [weak]() {
                auto self = weak.lock();
                return self && self->ShowSettingsWindow();
            },
            context);
        // Preserve the gate's pre-dispatch versus in-flight distinction. The
        // context state is not sufficient once the UI callback may have run.
        result.Status = ToUiActionStatus(uiResult);
        return result;
    };
    operations.ResourceStatus = [weak]() {
        apc::app::AppSnapshot::ResourceStatusSnapshot result;
        if (auto self = weak.lock()) {
            AdaptiveResourceDiagnostics diagnostics;
            {
                std::scoped_lock lock(self->m_resourceAuthorizationMutex);
                diagnostics = self->m_adaptiveResourceDiagnostics;
            }
            result.Evaluated = diagnostics.Evaluated;
            result.ForegroundResidency = ToAppResidency(diagnostics.Residency);
            result.BackgroundResidency = ToAppResidency(diagnostics.BackgroundResidency);
            result.SnapshotFresh = diagnostics.SnapshotFresh;
            result.PositiveAuthorizationCurrent = diagnostics.PositiveAuthorizationCurrent;
            result.PreloadAllowed = diagnostics.PreloadAllowed;
            result.UiResourcesLoaded = diagnostics.UiResourcesLoaded;
            result.UiResourcesInitialized = diagnostics.UiResourcesInitialized;
            result.Memory = ToAppMemoryPressure(diagnostics.Pressure.Memory);
            result.Activity = ToAppUserActivity(diagnostics.Pressure.UserActivity);
            result.EnergySaver = diagnostics.Pressure.EnergySaver;
        }
        return result;
    };
    operations.PickerOpenedGeneration = [weak]() {
        if (auto self = weak.lock(); self && self->m_trayController) {
            return self->m_trayController->DevicePickerOpenedGeneration();
        }
        return std::uint64_t{};
    };
    operations.Running = [weak]() {
        if (auto self = weak.lock()) return !self->m_exiting.load();
        return false;
    };
    operations.HasBusy = [weak]() {
        if (auto self = weak.lock(); self && self->m_deviceService) {
            return self->m_deviceService->HasBusyOperations();
        }
        return false;
    };
    operations.DeviceBusy = [weak](std::wstring_view deviceId) {
        if (auto self = weak.lock(); self && self->m_deviceService) {
            return self->m_deviceService->IsDeviceBusy(winrt::hstring(deviceId));
        }
        return false;
    };

    m_appBridge = std::make_shared<Bridge>(std::move(operations));
    std::weak_ptr<Bridge> weakBridge = m_appBridge;
    m_appController = std::make_unique<apc::app::AppController>(
        [weakBridge](apc::app::AppCommand const& command, apc::app::AppCommandContext const& context) {
            if (auto bridge = weakBridge.lock()) return bridge->Execute(command, context);
            apc::app::AppResult result;
            result.Code = apc::app::AppResultCode::Unavailable;
            result.Command = command.Kind;
            return result;
        },
        [weakBridge]() {
            if (auto bridge = weakBridge.lock()) return bridge->Snapshot();
            apc::app::AppSnapshot snapshot;
            snapshot.IsRunning = false;
            return snapshot;
        });
    m_controlCommandAdapter = std::make_unique<apc::control::ControlCommandAdapter>(
        *m_appController, apc::control::ControlCommandAdapter::Options{[](std::string_view key) { return _(key); }});
    DebugTrace(L"[App] AppController and control adapter initialized");
}

void ApplicationHost::InitializeCommandLineControl() {
    DebugTrace(L"[App] InitializeCommandLineControl()");
    auto weak = weak_from_this();
    m_commandLineControlServer.Start([weak](apc::control::Request const& request,
                                            std::stop_token stopToken,
                                            std::uint64_t deadline) -> apc::control::Response {
        if (auto self = weak.lock(); self && self->m_controlCommandAdapter) {
            return self->m_controlCommandAdapter->Handle(request, stopToken, deadline);
        }
        return {apc::control::ExitCode::Unavailable, L""};
    });
    DebugTrace(m_commandLineControlServer.IsRunning() ? L"[App] Command line control server started"
                                                      : L"[App] Command line control server retry scheduled");
}

void ApplicationHost::InitializeAdaptiveResources() noexcept {
    if (!m_trayController || !m_updateCoordinator) return;

    m_updateCoordinator->SetAutomaticChecksAllowed(false);
    try {
        auto weak = weak_from_this();
        m_trayController->SetResourceStateChangedCallback([weak](bool userInteraction) {
            if (auto self = weak.lock(); self && !self->m_exiting.load()) {
                self->EvaluateAdaptiveResources(userInteraction, L"tray-ui-state");
            }
        });

        m_resourcePressureMonitor =
            std::make_unique<ResourcePressureMonitor>([weak](ResourcePressureSnapshot const& value) {
                if (auto self = weak.lock()) {
                    if (value.Values.IsBackgroundConstrained()) {
                        std::scoped_lock authorizationLock(self->m_resourceAuthorizationMutex);
                        self->m_latestConstrainedResourcePressureSequence =
                            std::max(self->m_latestConstrainedResourcePressureSequence, value.Sequence);
                        if (self->m_updateCoordinator) {
                            self->m_updateCoordinator->SetAutomaticChecksAllowed(false);
                        }
                    }
                    self->HandleResourcePressureSnapshot(value);
                }
            });

        if (!m_resourcePressureMonitor->Start()) {
            m_resourcePressureMonitor.reset();
            DebugTrace(L"[App] Resource-pressure monitor unavailable; speculative preloading and automatic updates "
                       L"remain disabled");
        } else {
            m_powerSavingStatusNotification =
                RegisterPowerSettingNotification(m_hwnd, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (!m_powerSavingStatusNotification) {
                DebugTrace(L"[App] Battery-saver notification registration unavailable; polling fallback remains "
                           L"active");
            }
        }
    } catch (...) {
        m_resourcePressureMonitor.reset();
        OutputDebugStringW(L"[AudioPlaybackConnector2] Resource-pressure monitor initialization failed\n");
    }
    EvaluateAdaptiveResources(false, L"adaptive-startup");
}

void ApplicationHost::HandleResourcePressureSnapshot(ResourcePressureSnapshot snapshot) {
    auto weak = weak_from_this();
    static_cast<void>(RunOnUIThread([weak, snapshot = std::move(snapshot)]() mutable {
        auto self = weak.lock();
        if (!self || self->m_exiting.load() || snapshot.Sequence <= self->m_lastResourcePressureSequence) return;

        self->m_lastResourcePressureSequence = snapshot.Sequence;
        self->m_resourcePressureValues = snapshot.Values;
        self->m_lastResourcePressureObservedAt = snapshot.ObservedAt;
        self->EvaluateAdaptiveResources(false, L"resource-pressure-change");
    }));
}

void ApplicationHost::EvaluateAdaptiveResources(bool userInteraction, std::wstring_view reason) noexcept {
    if (userInteraction) m_adaptiveActionRetryBackoff.Reset();

    try {
        if (m_exiting.load() || !m_trayController || !m_hwnd || !IsWindow(m_hwnd)) return;

        auto const now = AdaptiveResourcePolicy::Clock::now();
        const bool snapshotFresh = IsResourcePressureSnapshotFresh(
            m_lastResourcePressureObservedAt, now, c_resourcePressureSnapshotMaximumAge);
        auto const pressureValues = snapshotFresh ? m_resourcePressureValues : ResourcePressureValues{};
        const bool energySaver = pressureValues.EnergySaver == true;
        const bool backgroundConstrained = pressureValues.IsBackgroundConstrained();
        std::unique_lock authorizationLock(m_resourceAuthorizationMutex);
        const bool positiveAuthorizationCurrent = IsPositiveResourceAuthorizationCurrent(
            m_lastResourcePressureSequence, m_latestConstrainedResourcePressureSequence);
        if (m_updateCoordinator) {
            m_updateCoordinator->SetAutomaticChecksAllowed(snapshotFresh && positiveAuthorizationCurrent &&
                                                           !backgroundConstrained);
        }
        AdaptiveResourcePolicyInput input{
            .MemoryPressure = pressureValues.IsMemoryPressure(),
            .PreloadAllowed = snapshotFresh && positiveAuthorizationCurrent && pressureValues.CanPreload(),
            .FullscreenOrPresentation = backgroundConstrained && !pressureValues.IsMemoryPressure() && !energySaver,
            .EnergySaver = energySaver,
            .UiVisible = m_trayController->IsDevicePickerVisibleOrTransitioning(),
            .UiPinned = false,
            .UserInteraction = userInteraction,
            .UiResourcesLoaded = m_trayController->IsDevicePickerLoaded(),
            .UiResourcesInitialized = m_trayController->IsDevicePickerPreloadInitialized(),
        };

        auto decision = m_adaptiveResourcePolicy.Evaluate(input, now);
        if (decision.ResidencyChanged || decision.BackgroundResidencyChanged ||
            decision.Action != AdaptiveResourceAction::None) {
            DebugTrace(L"[App] Adaptive resources reason={0} residency={1} background={2} action={3} memory={4} "
                       L"activity={5} energySaver={6}",
                       reason,
                       static_cast<int>(decision.Residency),
                       static_cast<int>(decision.BackgroundResidency),
                       static_cast<int>(decision.Action),
                       static_cast<int>(pressureValues.Memory),
                       static_cast<int>(pressureValues.UserActivity),
                       energySaver);
        }

        switch (decision.Action) {
            case AdaptiveResourceAction::PreloadUi: m_trayController->PreloadDevicePicker(); break;
            case AdaptiveResourceAction::ReleaseUi: m_trayController->ReleaseDevicePicker(); break;
            case AdaptiveResourceAction::None: break;
        }

        auto reevaluateAt = decision.ReevaluateAt;
        if (snapshotFresh && m_lastResourcePressureObservedAt) {
            auto const snapshotExpiry = *m_lastResourcePressureObservedAt + c_resourcePressureSnapshotMaximumAge;
            if (!reevaluateAt || snapshotExpiry < *reevaluateAt) reevaluateAt = snapshotExpiry;
        }
        const bool actionSucceeded =
            decision.Action == AdaptiveResourceAction::None ||
            (decision.Action == AdaptiveResourceAction::PreloadUi &&
             m_trayController->IsDevicePickerPreloadInitialized()) ||
            (decision.Action == AdaptiveResourceAction::ReleaseUi && !m_trayController->IsDevicePickerLoaded());
        if (!actionSucceeded) {
            auto const retryAt = AdaptiveResourcePolicy::Clock::now() + m_adaptiveActionRetryBackoff.RecordFailure();
            if (!reevaluateAt || retryAt < *reevaluateAt) reevaluateAt = retryAt;
        } else {
            m_adaptiveActionRetryBackoff.Reset();
        }
        m_adaptiveResourceDiagnostics = {
            .Evaluated = true,
            .Residency = decision.Residency,
            .BackgroundResidency = decision.BackgroundResidency,
            .Pressure = pressureValues,
            .SnapshotFresh = snapshotFresh,
            .PositiveAuthorizationCurrent = positiveAuthorizationCurrent,
            .PreloadAllowed = input.PreloadAllowed,
            .UiResourcesLoaded = m_trayController->IsDevicePickerLoaded(),
            .UiResourcesInitialized = m_trayController->IsDevicePickerPreloadInitialized(),
        };
        authorizationLock.unlock();
        ScheduleAdaptiveResourceEvaluation(reevaluateAt);
    } catch (...) {
        OutputDebugStringW(L"[AudioPlaybackConnector2] Adaptive resource evaluation failed\n");
        auto const retryAt = AdaptiveResourcePolicy::Clock::now() + m_adaptiveActionRetryBackoff.RecordFailure();
        ScheduleAdaptiveResourceEvaluation(retryAt);
    }
}

void ApplicationHost::ScheduleAdaptiveResourceEvaluation(
    std::optional<AdaptiveResourcePolicy::TimePoint> reevaluateAt) noexcept {
    auto const scheduleGeneration = m_adaptiveScheduleState.Supersede();
    if (m_adaptiveResourceFallbackTimer) {
        try {
            m_adaptiveResourceFallbackTimer.Stop();
        } catch (...) {
        }
        m_adaptiveResourceFallbackTimer = nullptr;
    }
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        static_cast<void>(m_adaptiveScheduleState.Consume(scheduleGeneration));
        return;
    }
    KillTimer(m_hwnd, c_timerAdaptiveResources);
    if (!reevaluateAt || m_exiting.load()) {
        static_cast<void>(m_adaptiveScheduleState.Consume(scheduleGeneration));
        return;
    }

    const auto now = AdaptiveResourcePolicy::Clock::now();
    auto remaining = *reevaluateAt > now ? *reevaluateAt - now : AdaptiveResourcePolicy::Clock::duration::zero();
    auto delay = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    delay = std::clamp<std::int64_t>(delay, 1, std::numeric_limits<UINT>::max());
    if (SetTimer(m_hwnd, c_timerAdaptiveResources, static_cast<UINT>(delay), nullptr)) {
        static_cast<void>(
            m_adaptiveScheduleState.SetWin32NotBefore(scheduleGeneration, now + std::chrono::milliseconds{delay}));
        return;
    }

    OutputDebugStringW(L"[AudioPlaybackConnector2] Win32 adaptive timer unavailable; using dispatcher fallback\n");
    try {
        if (!m_dispatcherQueue) return;
        auto timer = m_dispatcherQueue.CreateTimer();
        timer.Interval(std::chrono::milliseconds{delay});
        timer.IsRepeating(false);
        auto weak = weak_from_this();
        timer.Tick([weak, scheduleGeneration](auto const& sender, auto const&) noexcept {
            try {
                sender.Stop();
            } catch (...) {
            }
            if (auto self = weak.lock();
                self && !self->m_exiting.load() && self->m_adaptiveScheduleState.Consume(scheduleGeneration)) {
                self->m_adaptiveResourceFallbackTimer = nullptr;
                self->EvaluateAdaptiveResources(false, L"adaptive-dispatcher-deadline");
            }
        });
        m_adaptiveResourceFallbackTimer = timer;
        timer.Start();
    } catch (...) {
        m_adaptiveResourceFallbackTimer = nullptr;
        OutputDebugStringW(L"[AudioPlaybackConnector2] Failed to schedule adaptive resource fallback timer\n");
    }
}

winrt::hstring ApplicationHost::ResolveKnownDeviceName(winrt::hstring const& id) const {
    if (!m_settingsStore) return id;
    const auto settingsSnapshot = m_settingsStore->Snapshot();
    auto const& settings = settingsSnapshot.Data;
    auto it = std::ranges::find_if(settings.Devices, [&](const auto& device) { return device.Id == id; });
    if (it != settings.Devices.end()) {
        if (!it->Alias.empty()) return winrt::hstring(it->Alias);
        if (settings.PrivacyModeEnabled) return winrt::hstring(_("Privacy_RedactedDevice"));
        if (!it->Name.empty()) return winrt::hstring(it->Name);
    }
    return settings.PrivacyModeEnabled ? winrt::hstring(_("Privacy_RedactedDevice")) : id;
}

void ApplicationHost::ExecuteTrayCommand(apc::app::AppCommand command,
                                         apc::app::AppCommandContext::CompletionMode completion) {
    if (m_exiting.load() || !m_appController) return;

    const auto kind = command.Kind;
    apc::app::AppCommandContext context;
    context.Completion = completion;
    const auto result = m_appController->Execute(std::move(command), context);
    if (kind == apc::app::AppCommandKind::ToggleLast && result.Code == apc::app::AppResultCode::Success) {
        ScheduleDeviceVisualRefresh(false);
    }
}

void ApplicationHost::TryAutoReconnect() {
    if (m_exiting.load() || !m_settingsStore || !m_deviceService) return;

    DebugTrace(L"[App] TryAutoReconnect()");
    const auto settingsSnapshot = m_settingsStore->Snapshot();
    const auto reconnectIds = AutoReconnectPlanner::BuildReconnectPlan(settingsSnapshot.Data);

    for (const auto& id : reconnectIds) {
        DebugTrace(L"[App] Auto-reconnecting to: {0}", id);
    }
    m_deviceService->ConnectStartupTargets(reconnectIds);
}

void ApplicationHost::HandlePowerSuspend() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleSuspend(
        [weak]() {
            if (auto self = weak.lock()) {
                if (!self->m_settingsStore || self->m_settingsStore->FlushNow(3)) return;
                DebugTrace(L"[App] SettingsStore synchronous suspend flush failed after bounded attempts");
            }
        },
        m_deviceService);
}

void ApplicationHost::HandlePowerResume() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleResume(
        m_deviceService,
        [weak](std::vector<std::wstring> deviceIds,
               std::uint64_t generation,
               PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
            auto completionUsed = std::make_shared<std::atomic_bool>(false);
            auto finish = [completed = std::move(completed),
                           completionUsed](std::vector<std::wstring> attemptedIds) mutable noexcept {
                if (!completionUsed->exchange(true) && completed) completed(std::move(attemptedIds));
            };

            auto self = weak.lock();
            if (!self) {
                finish({});
                return;
            }
            auto accepted = self->RunOnUIThread([weak, generation, deviceIds = std::move(deviceIds), finish]() mutable {
                std::vector<std::wstring> attemptedIds;
                auto completionGuard = wil::scope_exit([&]() noexcept { finish(std::move(attemptedIds)); });
                auto self = weak.lock();
                if (!self || !self->m_deviceService ||
                    !self->m_powerTransitionCoordinator.IsResumeReconnectGenerationCurrent(generation)) {
                    return;
                }
                for (auto const& deviceId : deviceIds) {
                    if (deviceId.empty() || self->m_deviceService->IsDeviceConnected(winrt::hstring(deviceId)) ||
                        self->m_deviceService->IsDeviceBusy(winrt::hstring(deviceId))) {
                        continue;
                    }
                    self->m_deviceService->ConnectDetached(winrt::hstring(deviceId));
                    attemptedIds.push_back(deviceId);
                }
            });
            if (!accepted) finish({});
        });
}

winrt::fire_and_forget ApplicationHost::CheckForUpdatesOnStartupAsync() {
    try {
        auto lifetime = shared_from_this();
        auto settingsStore = m_settingsStore;
        auto notificationService = m_notificationService;
        auto updateCoordinator = m_updateCoordinator;
        if (m_exiting.load() || !settingsStore || !notificationService || !updateCoordinator) co_return;
        co_await StartupUpdateCoordinator::CheckForUpdatesAsync(
            *settingsStore, notificationService, updateCoordinator, m_exiting);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Startup update check failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Startup update check failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Startup update check failed");
    }
}

bool ApplicationHost::RunOnUIThread(std::function<void()> work) noexcept {
    if (m_exiting.load() || !work) return false;

    bool hasThreadAccess = false;
    try {
        hasThreadAccess = m_dispatcherQueue && m_dispatcherQueue.HasThreadAccess();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Failed to query DispatcherQueue thread access", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Failed to query DispatcherQueue thread access", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Failed to query DispatcherQueue thread access");
    }

    if (hasThreadAccess) {
        if (m_exiting.load()) return false;
        try {
            work();
            return true;
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Inline UI work failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Inline UI work failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Inline UI work failed");
        }
        return false;
    }

    try {
        if (m_dispatcherQueue) {
            auto weak = weak_from_this();
            auto dispatcherWork = work;
            if (m_dispatcherQueue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
                                             [weak, work = std::move(dispatcherWork)]() mutable noexcept {
                                                 try {
                                                     if (auto self = weak.lock(); self && !self->m_exiting.load()) {
                                                         work();
                                                     }
                                                 } catch (winrt::hresult_error const& ex) {
                                                     util::DebugTraceException(L"[App] UI-dispatched work failed", ex);
                                                 } catch (std::exception const& ex) {
                                                     util::DebugTraceException(L"[App] UI-dispatched work failed", ex);
                                                 } catch (...) {
                                                     util::DebugTraceUnknownException(
                                                         L"[App] UI-dispatched work failed");
                                                 }
                                             })) {
                return true;
            }
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Dispatcher queue rejected UI work", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Dispatcher queue rejected UI work", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Dispatcher queue rejected UI work");
    }

    return QueueUiFallbackWork(std::move(work));
}

bool ApplicationHost::QueueUiFallbackWork(std::function<void()> work) noexcept {
    if (m_exiting.load() || !work) return false;

    try {
        std::scoped_lock lock(m_uiFallbackWorkMutex);
        if (m_exiting.load()) return false;
        m_uiFallbackWork.push_back(std::move(work));
        if (m_uiFallbackMessagePending) return true;
        if (m_hwnd && IsWindow(m_hwnd) && PostMessageW(m_hwnd, c_messageDrainUiFallbackWork, 0, 0)) {
            m_uiFallbackMessagePending = true;
            return true;
        }
        m_uiFallbackWork.clear();
    } catch (...) {
    }
    DebugTrace(L"[App] ERROR: both DispatcherQueue and Win32 fallback rejected UI work");
    return false;
}

void ApplicationHost::DrainUiFallbackWork() noexcept {
    std::deque<std::function<void()>> workItems;
    {
        std::scoped_lock lock(m_uiFallbackWorkMutex);
        workItems.swap(m_uiFallbackWork);
        m_uiFallbackMessagePending = false;
    }

    for (auto& work : workItems) {
        try {
            if (!m_exiting.load() && work) work();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Win32-fallback UI work failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Win32-fallback UI work failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Win32-fallback UI work failed");
        }
    }
}

ApplicationHost::ControlUiActionResult ApplicationHost::RunControlUiAction(std::function<bool()> work,
                                                                           apc::app::AppCommandContext const& context) {
    if (m_exiting.load() || !m_dispatcherQueue || context.IsCancellationRequested()) {
        return ControlUiActionResult::Failed;
    }
    if (m_dispatcherQueue.HasThreadAccess()) {
        try {
            return !m_exiting.load() && work() ? ControlUiActionResult::Succeeded : ControlUiActionResult::Failed;
        } catch (...) {
            return ControlUiActionResult::Failed;
        }
    }

    struct ActionState {
        ActionState() { Completed.create(); }
        wil::unique_event Completed;
        ControlUiActionGate Gate;
    };
    auto state = std::make_shared<ActionState>();
    auto weak = weak_from_this();
    if (!m_dispatcherQueue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
                                      [weak, state, work = std::move(work)]() mutable noexcept {
                                          if (!state->Gate.TryBegin()) {
                                              state->Completed.SetEvent();
                                              return;
                                          }
                                          bool succeeded = false;
                                          try {
                                              auto self = weak.lock();
                                              if (self && !self->m_exiting.load()) {
                                                  succeeded = work();
                                              }
                                          } catch (...) {
                                          }
                                          state->Gate.Complete(succeeded);
                                          state->Completed.SetEvent();
                                      })) {
        return ControlUiActionResult::Failed;
    }

    while (true) {
        if (context.IsCancellationRequested() || m_exiting.load()) {
            return state->Gate.CancelOrClassify();
        }
        DWORD remaining = INFINITE;
        if (context.Deadline != apc::app::AppCommandContext::TimePoint::max()) {
            const auto duration = context.Deadline - apc::app::AppCommandContext::Clock::now();
            if (duration <= std::chrono::steady_clock::duration::zero()) return state->Gate.CancelOrClassify();
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            remaining = static_cast<DWORD>(std::clamp<std::int64_t>(milliseconds, 1, INFINITE));
        }
        const auto waitResult = WaitForSingleObject(state->Completed.get(), std::min<DWORD>(remaining, 100));
        if (waitResult == WAIT_OBJECT_0) {
            return state->Gate.CurrentResult();
        }
        if (waitResult != WAIT_TIMEOUT) return state->Gate.CancelOrClassify();
    }
}

bool ApplicationHost::RefreshTrayVisualState(bool forceErrorWhenIdle, std::wstring_view reason) {
    if (m_exiting.load() || !m_trayController || !m_deviceService) {
        DebugTrace(L"[App] RefreshTrayVisualState skipped reason={0} exiting={1} hasTrayController={2} "
                   L"hasDeviceService={3}",
                   reason,
                   m_exiting.load(),
                   m_trayController != nullptr,
                   m_deviceService != nullptr);
        return true;
    }
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        DebugTrace(L"[App] RefreshTrayVisualState skipped reason={0} invalidHwnd hwnd=0x{1:X}",
                   reason,
                   reinterpret_cast<uintptr_t>(m_hwnd));
        return true;
    }

    auto const presentation = m_deviceService->GetTrayPresentationSnapshot();
    const bool hasBusyOperations = presentation.HasBusyOperations;
    const bool hasConnections = !presentation.ConnectedDevices.empty();
    const auto now = std::chrono::steady_clock::now();
    if (forceErrorWhenIdle) {
        m_trayErrorUntil = now + std::chrono::milliseconds(c_transientTrayErrorMs);
    }
    if (hasConnections) {
        m_trayErrorUntil = {};
        m_transientTrayErrorTooltip.clear();
    }
    const bool showTransientError = !hasConnections && !hasBusyOperations && now < m_trayErrorUntil;

    TrayIconState desiredState = TrayIconState::Idle;
    bool timersReady = true;
    if (hasConnections) {
        desiredState = TrayIconState::Connected;
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        KillTimer(m_hwnd, c_timerTransientTrayError);
    } else if (hasBusyOperations) {
        desiredState = TrayIconState::Connecting;
        if (!m_connectingAnimationTimerActive) {
            if (SetTimer(m_hwnd, c_timerAnimation, 75, nullptr)) {
                m_connectingAnimationTimerActive = true;
            } else {
                timersReady = false;
                DebugTrace(L"[App] Connecting animation timer unavailable: {0}", GetLastError());
            }
        }
    } else if (showTransientError) {
        desiredState = TrayIconState::Error;
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_trayErrorUntil - now);
        const auto delay = static_cast<UINT>(std::clamp<std::int64_t>(remaining.count(), 1, UINT_MAX));
        if (!SetTimer(m_hwnd, c_timerTransientTrayError, delay, nullptr)) {
            timersReady = false;
            DebugTrace(L"[App] Transient tray error timer unavailable: {0}", GetLastError());
        }
    } else {
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        KillTimer(m_hwnd, c_timerTransientTrayError);
        m_trayErrorUntil = {};
    }

    DebugTrace(L"[App] RefreshTrayVisualState reason={0} forceErrorWhenIdle={1} hasConnections={2} "
               L"hasBusyOperations={3} transientError={4} desired={5}",
               reason,
               forceErrorWhenIdle,
               hasConnections,
               hasBusyOperations,
               showTransientError,
               TrayIconStateToString(desiredState));
    if (showTransientError && !m_transientTrayErrorTooltip.empty()) {
        m_trayController->UpdateTooltip(m_transientTrayErrorTooltip);
    } else {
        m_trayController->UpdateTooltipFromConnections(presentation.ConnectedDevices);
    }
    m_trayController->SetState(desiredState);
    auto const pickerUpdated = m_trayController->RefreshDevicePickerState();
    auto const shellUpdated = m_trayController->ApplyPendingTrayUpdates();
    if (!shellUpdated && m_connectingAnimationTimerActive) {
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
    }
    return pickerUpdated && shellUpdated && timersReady;
}

void ApplicationHost::ScheduleDeviceVisualRefresh(bool forceErrorWhenIdle, bool inventoryChanged, bool refreshTray) {
    auto flags = (refreshTray || forceErrorWhenIdle ? c_visualRefreshRequested : 0U) |
                 (forceErrorWhenIdle ? c_visualRefreshForceError : 0U) |
                 (inventoryChanged ? c_visualRefreshInventoryChanged : 0U);
    if (flags == 0) return;
    if (m_deviceVisualRefreshCoalescer.Request(flags)) {
        QueueDeviceVisualRefreshDrain();
    }
}

void ApplicationHost::QueueDeviceVisualRefreshDrain() noexcept {
    constexpr unsigned int c_maxImmediateScheduleAttempts = 3;
    for (unsigned int attempt = 0; attempt < c_maxImmediateScheduleAttempts; ++attempt) {
        if (m_exiting.load()) {
            m_deviceVisualRefreshCoalescer.Cancel();
            return;
        }

        if (m_dispatcherQueue) {
            try {
                auto weak = weak_from_this();
                if (m_dispatcherQueue.TryEnqueue([weak]() {
                        if (auto self = weak.lock()) self->DrainDeviceVisualRefresh();
                    })) {
                    return;
                }
                DebugTrace(L"[App] Dispatcher rejected coalesced device visual refresh");
            } catch (winrt::hresult_error const& ex) {
                util::DebugTraceException(L"[App] Failed to enqueue coalesced device visual refresh", ex);
            } catch (std::exception const& ex) {
                util::DebugTraceException(L"[App] Failed to enqueue coalesced device visual refresh", ex);
            } catch (...) {
                util::DebugTraceUnknownException(L"[App] Failed to enqueue coalesced device visual refresh");
            }
        }

        if (m_hwnd && IsWindow(m_hwnd)) {
            if (PostMessageW(m_hwnd, c_messageDrainDeviceVisualRefresh, 0, 0)) return;
            if (SetTimer(m_hwnd, c_timerDeviceVisualRefreshRetry, 50, nullptr)) return;
        }

        if (ScheduleNativeDeviceVisualRefreshRetry(std::chrono::milliseconds(100))) return;

        if (!m_deviceVisualRefreshCoalescer.ScheduleFailed()) return;
    }

    DebugTrace(L"[App] ERROR: no UI scheduling path accepted the device visual refresh");
    m_deviceVisualRefreshCoalescer.AbandonSchedule();
}

void ApplicationHost::DrainDeviceVisualRefresh() noexcept {
    auto flags = m_deviceVisualRefreshCoalescer.BeginDrain();
    bool succeeded = false;
    try {
        if (!m_exiting.load()) {
            succeeded = true;
            if ((flags & c_visualRefreshInventoryChanged) != 0 && m_trayController) {
                succeeded = m_trayController->InvalidateDevicePickerInventory();
            }
            if (succeeded && (flags & c_visualRefreshRequested) != 0) {
                succeeded =
                    RefreshTrayVisualState((flags & c_visualRefreshForceError) != 0, L"coalesced-device-events");
            }
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Coalesced device visual refresh failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Coalesced device visual refresh failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Coalesced device visual refresh failed");
    }

    if (m_exiting.load()) {
        m_deviceVisualRefreshCoalescer.Cancel();
        return;
    }

    if (!succeeded) {
        static_cast<void>(m_deviceVisualRefreshCoalescer.Request(flags & ~c_visualRefreshForceError));
    }

    if (!m_deviceVisualRefreshCoalescer.CompleteDrain()) {
        if (succeeded) m_deviceVisualRefreshConsecutiveFailures = 0;
        return;
    }

    if (succeeded) {
        m_deviceVisualRefreshConsecutiveFailures = 0;
        QueueDeviceVisualRefreshDrain();
        return;
    }

    m_deviceVisualRefreshConsecutiveFailures = std::min(m_deviceVisualRefreshConsecutiveFailures + 1U, 6U);
    auto const delay = std::min<UINT>(100U << (m_deviceVisualRefreshConsecutiveFailures - 1U), 5000U);
    if (m_hwnd && IsWindow(m_hwnd) && SetTimer(m_hwnd, c_timerDeviceVisualRefreshRetry, delay, nullptr)) {
        DebugTrace(L"[App] Device visual refresh retry scheduled in {0} ms", delay);
    } else {
        QueueDeviceVisualRefreshDrain();
    }
}

void ApplicationHost::SetupDeviceEvents() {
    DebugTrace(L"[App] SetupDeviceEvents()");
    auto weak = weak_from_this();
    DeviceEventRouter::Callbacks callbacks;
    callbacks.DeviceConnected = [weak](auto const& id) {
        if (auto self = weak.lock()) {
            // The router rejects superseded queued events. Recheck the authoritative session at
            // publication time too, because DeviceConnected carries a state overlay into the
            // controller snapshot and must never revive a closed session.
            if (!self->m_deviceService || !self->m_deviceService->IsDeviceConnected(id)) return;
            self->OnDeviceConnected(id);
            self->PublishDeviceFact({.Kind = Bridge::FactKind::DeviceConnected, .Id = std::wstring(id)});
        }
    };
    callbacks.DeviceDisconnected = [weak](auto const& id) {
        if (auto self = weak.lock()) {
            self->OnDeviceDisconnected(id);
            self->PublishDeviceFact({.Kind = Bridge::FactKind::DeviceDisconnected, .Id = std::wstring(id)});
        }
    };
    callbacks.ConnectionError = [weak](auto const& id, auto const& msg) {
        if (auto self = weak.lock()) {
            self->OnConnectionError(id, msg);
            self->PublishDeviceFact({.Kind = Bridge::FactKind::ConnectionError,
                                     .Id = std::wstring(id),
                                     .ErrorCode = apc::app::AppResultCode::OperationFailed});
        }
    };
    callbacks.AutoReconnectTriggered = [weak](auto const& id) {
        if (auto self = weak.lock()) {
            self->OnAutoReconnectTriggered(id);
            self->PublishDeviceFact({.Kind = Bridge::FactKind::AutoReconnectTriggered, .Id = std::wstring(id)});
        }
    };
    callbacks.AutoReconnectFailed = [weak](auto const& id) {
        if (auto self = weak.lock()) {
            self->OnAutoReconnectFailed(id);
            self->PublishDeviceFact({.Kind = Bridge::FactKind::AutoReconnectFailed, .Id = std::wstring(id)});
        }
    };
    callbacks.DeviceStatusChanged = [weak](auto const& id, auto const&, DeviceStatusKind statusKind) {
        auto self = weak.lock();
        if (!self) return;
        if (!self->m_exiting.load() && self->m_trayController && self->m_deviceService && self->m_hwnd &&
            IsWindow(self->m_hwnd)) {
            self->ScheduleDeviceVisualRefresh(statusKind == DeviceStatusKind::Error);
        }
        self->PublishDeviceFact({.Kind = Bridge::FactKind::DeviceStatusChanged,
                                 .Id = std::wstring(id),
                                 .State = ToAppDeviceState(statusKind)});
    };
    callbacks.DeviceActivityChanged = [weak]() {
        auto self = weak.lock();
        if (!self) return;
        if (!self->m_exiting.load() && self->m_trayController && self->m_deviceService && self->m_hwnd &&
            IsWindow(self->m_hwnd)) {
            self->ScheduleDeviceVisualRefresh(false);
        }
        self->PublishDeviceFact({.Kind = Bridge::FactKind::DeviceActivityChanged});
    };
    callbacks.DeviceInventoryChanged = [weak]() {
        auto self = weak.lock();
        if (!self) return;
        if (!self->m_exiting.load() && self->m_trayController && self->m_deviceService && self->m_hwnd &&
            IsWindow(self->m_hwnd)) {
            self->ScheduleDeviceVisualRefresh(false, true, false);
        }
        self->PublishDeviceFact({.Kind = Bridge::FactKind::DeviceInventoryChanged});
    };

    m_deviceEventRouter.Attach(
        m_deviceService,
        [weak](std::function<void()> work) {
            if (auto self = weak.lock()) {
                return self->RunOnUIThread(std::move(work));
            }
            return false;
        },
        std::move(callbacks));
}

void ApplicationHost::TeardownDeviceEvents() {
    m_deviceEventRouter.Detach();
}

void ApplicationHost::PublishDeviceFact(Bridge::DeviceFact fact) noexcept {
    if (!m_appBridge || !m_appController) return;
    if (auto event = m_appBridge->Observe(std::move(fact))) {
        m_appController->Publish(*event);
    }
}

bool ApplicationHost::ShowSettingsWindow() {
    if (m_exiting.load()) return false;
    DebugTrace(L"[App] ShowSettingsWindow()");
    auto weak = weak_from_this();
    return m_settingsWindowPresenter.Show(
        m_settingsController,
        [weak](apc::app::AppCommand command) {
            if (auto self = weak.lock(); self && !self->m_exiting.load() && self->m_appController) {
                return self->m_appController->Execute(std::move(command));
            }
            apc::app::AppResult result;
            result.Code = apc::app::AppResultCode::Unavailable;
            result.Command = command.Kind;
            result.Reason = apc::app::AppOutcomeReason::NotReady;
            return result;
        },
        m_startupTaskCoordinator,
        m_trayController,
        m_updateCoordinator);
}

void ApplicationHost::ExitApplication() noexcept {
    DebugTrace(L"[App] ExitApplication() started");
    auto const settingsWindowClosed = PerformTeardown(SettingsShutdownMode::Flush);
    auto const mainWindowClosed = CloseMainWindow(L"application-exit");
    if (!settingsWindowClosed || !mainWindowClosed) TerminateAfterWindowCloseFailure(L"exit-window-close-failure");
    DebugTrace(L"[App] ExitApplication() complete");
}

bool ApplicationHost::CloseMainWindow(std::wstring_view reason) noexcept {
    if (!m_mainWindow) return true;
    try {
        m_mainWindow.Close();
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason), ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason), ex);
    } catch (...) {
        util::DebugTraceUnknownException(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason));
    }
    return false;
}

/*//////// Device Event Handlers /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::OnDeviceConnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_deviceService) return;
    DebugTrace(L"[App] OnDeviceConnected: {0}", std::wstring(id));

    if (!m_deviceService->IsDeviceConnected(id)) {
        return;
    }
    m_powerTransitionCoordinator.NotifyDeviceConnected(std::wstring_view(id));
    if (!m_settingsStore) return;

    winrt::hstring rawDeviceName = id;
    if (auto displayName = m_deviceService->GetConnectionDisplayName(id)) {
        rawDeviceName = winrt::hstring(*displayName);
    }

    auto const idString = std::wstring(id);
    auto deviceName =
        apc::limits::TruncateUtf16(std::wstring_view(rawDeviceName), apc::limits::c_maxDeviceNameCharacters);
    if (deviceName.empty()) {
        deviceName = apc::limits::TruncateUtf16(idString, apc::limits::c_maxDeviceNameCharacters);
    }
    RecordConnectedDeviceResult record;
    try {
        record = m_settingsStore->RecordConnectedDevice(idString, deviceName);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] OnDeviceConnected settings update ERROR", ex);
        return;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] OnDeviceConnected settings update ERROR", ex);
        return;
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] OnDeviceConnected settings update ERROR");
        return;
    }

    if (record.Mutation.IsApplied()) {
        if (record.PresentationChanged) m_settingsWindowPresenter.RefreshKnownDevicesIfOpen();
        if (record.AddedDevice) {
            DebugTrace(L"[App] New device added to settings: {0}", std::wstring(rawDeviceName));
        }
    }

    try {
        const auto settingsSnapshot = m_settingsStore->Snapshot();
        bool reconnectOnConnectionLoss = settingsSnapshot.Data.GlobalReconnectOnConnectionLoss;
        auto it = std::ranges::find_if(settingsSnapshot.Data.Devices, [&](const auto& d) { return d.Id == id; });
        if (it != settingsSnapshot.Data.Devices.end()) {
            reconnectOnConnectionLoss = reconnectOnConnectionLoss || it->ReconnectOnConnectionLoss;
        }
        m_deviceService->SetReconnectOnConnectionLoss(id, reconnectOnConnectionLoss);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] OnDeviceConnected reconnect-on-loss sync ERROR", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] OnDeviceConnected reconnect-on-loss sync ERROR", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] OnDeviceConnected reconnect-on-loss sync ERROR");
    }

    if (m_notificationService) {
        try {
            m_notificationService->ShowDeviceConnected(id, ResolveKnownDeviceName(id));
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] OnDeviceConnected notification ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] OnDeviceConnected notification ERROR", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] OnDeviceConnected notification ERROR");
        }
    }

    ScheduleDeviceVisualRefresh(false);
}

void ApplicationHost::OnDeviceDisconnected(winrt::hstring const& id) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnDeviceDisconnected: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    if (m_notificationService) {
        try {
            m_notificationService->ShowDeviceDisconnected(id, deviceName);
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] OnDeviceDisconnected notification ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] OnDeviceDisconnected notification ERROR", ex);
        }
    }

    ScheduleDeviceVisualRefresh(false);
}

void ApplicationHost::OnConnectionError(winrt::hstring const& id, winrt::hstring msg) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnConnectionError: {0} - {1}", std::wstring(id), std::wstring(msg));
    m_transientTrayErrorTooltip = std::wstring(_("AppName")) + L"\n" + std::wstring(msg);
    ScheduleDeviceVisualRefresh(true);
}

void ApplicationHost::OnAutoReconnectTriggered(winrt::hstring const& id) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnAutoReconnectTriggered: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    if (m_notificationService) {
        try {
            m_notificationService->ShowAutoReconnect(id, deviceName);
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] OnAutoReconnectTriggered notification ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] OnAutoReconnectTriggered notification ERROR", ex);
        }
    }
}

void ApplicationHost::OnAutoReconnectFailed(winrt::hstring const& id) {
    if (m_exiting.load()) return;
    DebugTrace(L"[App] OnAutoReconnectFailed: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    if (m_notificationService) {
        try {
            m_notificationService->ShowAutoReconnectFailed(id, deviceName);
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] OnAutoReconnectFailed notification ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] OnAutoReconnectFailed notification ERROR", ex);
        }
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Window Subclass ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

LRESULT CALLBACK ApplicationHost::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) noexcept try {
    auto* host = reinterpret_cast<ApplicationHost*>(dwRefData);
    if (!host) return DefSubclassProc(hwnd, msg, wParam, lParam);
    if (host->m_exiting.load()) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (host->m_trayController && msg == host->m_trayController->TrayCallbackMessage()) {
        host->m_trayController->HandleTrayMessage(wParam, lParam);
        return 0;
    }

    if (msg == c_messageDrainUiFallbackWork) {
        host->DrainUiFallbackWork();
        return 0;
    }

    if (msg == c_messageDrainDeviceVisualRefresh) {
        host->DrainDeviceVisualRefresh();
        return 0;
    }

    if (msg == WM_SETTINGCHANGE) {
        ThemeHelper::OnSettingChange(hwnd, lParam);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_POWERBROADCAST) {
        switch (wParam) {
            case PBT_APMSUSPEND: host->HandlePowerSuspend(); return TRUE;
            case PBT_APMRESUMEAUTOMATIC:
            case PBT_APMRESUMESUSPEND:
                host->HandlePowerResume();
                if (host->m_resourcePressureMonitor) {
                    static_cast<void>(host->m_resourcePressureMonitor->RequestProbe());
                }
                return TRUE;
            case PBT_POWERSETTINGCHANGE:
                if (host->m_resourcePressureMonitor) {
                    static_cast<void>(host->m_resourcePressureMonitor->RequestProbe());
                }
                return TRUE;
            default: break;
        }
    }

    if (msg == WM_TIMER && wParam == c_timerAnimation && host->m_trayController) {
        if (!host->m_trayController->AdvanceConnectingFrame()) {
            KillTimer(hwnd, c_timerAnimation);
            host->m_connectingAnimationTimerActive = false;
            host->ScheduleDeviceVisualRefresh(false);
        }
        return 0;
    }

    if (msg == WM_TIMER && wParam == c_timerTransientTrayError) {
        KillTimer(hwnd, c_timerTransientTrayError);
        host->ScheduleDeviceVisualRefresh(false);
        return 0;
    }

    if (msg == WM_TIMER && wParam == c_timerAdaptiveResources) {
        if (!host->m_adaptiveScheduleState.ConsumeWin32IfDue(AdaptiveResourcePolicy::Clock::now())) return 0;
        KillTimer(hwnd, c_timerAdaptiveResources);
        host->EvaluateAdaptiveResources(false, L"adaptive-deadline");
        return 0;
    }

    if (msg == WM_TIMER && wParam == c_timerDeviceVisualRefreshRetry) {
        KillTimer(hwnd, c_timerDeviceVisualRefreshRetry);
        host->DrainDeviceVisualRefresh();
        return 0;
    }

    if (s_wmTaskbarCreated && msg == s_wmTaskbarCreated) {
        if (host->m_trayController) {
            host->m_trayController->Reregister();
            host->m_trayController->OnThemeChanged();
            host->ScheduleDeviceVisualRefresh(false);
        }
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
} catch (...) {
    OutputDebugStringW(L"[AudioPlaybackConnector2] Window subclass callback failed\n");
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
