#include <pch.h>

#include <app/ApplicationHost.hpp>
#include <type_traits>

#include <MainWindow/MainWindow.xaml.h>
#include <core/DeviceService.hpp>
#include <core/SettingsStore.hpp>
#include <core/StringResources.hpp>
#include <core/ThemeHelper.hpp>
#include <core/TrayTooltipBuilder.hpp>
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

constexpr int c_hiddenAnchorCoordinate = -32000;
constexpr auto c_resourcePressureSnapshotMaximumAge = std::chrono::seconds{75};
constexpr auto c_mainWindowLoadedTimeout = std::chrono::seconds{15};

using OperationStatus = apc::app::AppActionStatus;

OperationStatus ToUiActionStatus(ControlUiActionGate::Result result) noexcept {
    switch (result) {
        case ControlUiActionGate::Result::Succeeded: return OperationStatus::Succeeded;
        case ControlUiActionGate::Result::Failed: return OperationStatus::Failed;
        case ControlUiActionGate::Result::Indeterminate: return OperationStatus::Indeterminate;
    }
    return OperationStatus::Failed;
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

void LogMainWindowAnchor(util::LogSink const& log, HWND hwnd, std::wstring_view reason) noexcept {
    if (!hwnd) return;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    auto const style = GetWindowLongPtr(hwnd, GWL_STYLE);
    auto const exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    log.Trace(L"[App] MainWindow anchor reason={0} hwnd=0x{1:X} visible={2} rect=({3},{4})-({5},{6}) "
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

void ConfigureHiddenMainWindowAnchor(util::LogSink const& log, HWND hwnd) noexcept {
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
    LogMainWindowAnchor(log, hwnd, L"configured-hidden-anchor");
}

[[noreturn]] void TerminateAfterWindowCloseFailure(util::EmergencyLog const& emergency,
                                                   std::wstring_view reason) noexcept {
    (void)emergency.Dump(reason, ERROR_PROCESS_ABORTED);
    ExitProcess(ERROR_PROCESS_ABORTED);
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

ApplicationHost::ApplicationHost(util::LogSink log, util::EmergencyLog emergency)
    : m_log(std::move(log)), m_emergencyLog(std::move(emergency)) {}

ApplicationHost::~ApplicationHost() {
    Shutdown();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::Start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        m_log.Trace(L"[App] Start ignored because initialization already began");
        return;
    }
    if (!m_singleInstanceGuard.TryAcquire(L"AudioPlaybackConnector2_SingleInstance_v2")) {
        ExitProcess(0);
        return;
    }
    m_log.Trace(L"[App] OnLaunched started");
    try {
        SetupMainWindow();
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[App] Main window setup failed", ex);
        FailStartup(L"main-window-setup-hresult");
    } catch (std::exception const& ex) {
        m_log.Exception(L"[App] Main window setup failed", ex);
        FailStartup(L"main-window-setup-standard");
    } catch (...) {
        m_log.UnknownException(L"[App] Main window setup failed");
        FailStartup(L"main-window-setup-unknown");
    }
}

bool ApplicationHost::PerformTeardown(SettingsShutdownMode settingsShutdownMode) noexcept {
    if (m_exiting.exchange(true)) return m_teardownWindowCloseSucceeded.load();
    // Request pipe-handler cancellation before waiting for controller calls. A
    // handler may be waiting for this UI thread to process show/settings work;
    // Stop() still drains the server after the controller has rejected new work.
    m_commandLineControlServer.RequestStop();
    // Persist the window's final placement before closing application admission.
    auto const settingsWindowClosed = m_settingsWindowPresenter.Close();
    m_teardownWindowCloseSucceeded.store(settingsWindowClosed);
    if (m_appController) {
        m_appController->Shutdown();
    }

    StopMainWindowLoadedWatchdog();
    if (auto notification = std::exchange(m_powerSavingStatusNotification, nullptr)) {
        if (!UnregisterPowerSettingNotification(notification)) {
            m_log.Trace(L"[App] Failed to unregister battery-saver notification: {0}", GetLastError());
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
    if (m_startupTaskCoordinator) {
        m_startupTaskCoordinator->Shutdown();
    }
    m_commandLineControlServer.Stop();
    m_controlCommandAdapter.reset();
    TeardownDeviceEvents();
    m_appController.reset();
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
                    m_log.Trace(L"[App] ERROR: failed to remove MainWindow subclass: {0}", GetLastError());
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
        m_deviceService->Shutdown();
        m_deviceService.reset();
    }
    m_notificationService.reset();
    m_startupTaskCoordinator.reset();
    m_trayController.reset();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
    if (m_settingsStore) {
        const auto settingsShutdown = m_settingsStore->Shutdown(settingsShutdownMode, 3);
        if (!settingsShutdown) {
            m_log.Trace(L"[App] SettingsStore shutdown failed mode={0}",
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
        m_log.Trace(L"[App] ERROR: MainWindow.Content() is null!");
        FailStartup(L"main-window-content-null");
        return;
    }

    auto root = content.try_as<Controls::Grid>();
    if (!root) {
        m_log.Trace(L"[App] ERROR: MainWindow.Content() is not a Grid!");
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
    m_log.Trace(L"[App] MainWindow.Activate() called");
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
        m_log.Trace(L"[App] Grid.Loaded fired again, ignoring (already initialized)");
        return;
    }
    StopMainWindowLoadedWatchdog();
    m_log.Trace(L"[App] Grid.Loaded - beginning initialization");

    m_hwnd = util::GetWindowHandle(m_mainWindow);
    if (!m_hwnd) {
        m_log.Trace(L"[App] ERROR: GetWindowHandle returned null!");
        FailStartup(L"main-window-hwnd-null");
        return;
    }
    m_log.Trace(L"[App] MainWindow HWND = 0x{0:X}", reinterpret_cast<uintptr_t>(m_hwnd));

    ConfigureHiddenMainWindowAnchor(m_log, m_hwnd);
    root.Opacity(1);
    m_log.Trace(L"[App] MainWindow hidden anchor configured");

    if (!SetWindowSubclass(m_hwnd, SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
        m_log.Trace(L"[App] ERROR: SetWindowSubclass failed: {0}", GetLastError());
        FailStartup(L"main-window-subclass");
        return;
    }
    m_windowSubclassInstalled = true;
    m_log.Trace(L"[App] Window subclass installed");

    m_settingsStore = std::make_shared<SettingsStore>(std::filesystem::path{}, nullptr, nullptr, m_log);
    m_settingsStore->Load();
    m_log.Trace(L"[App] Settings loaded");

    const auto settingsSnapshot = m_settingsStore->Snapshot();
    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), settingsSnapshot.Data.Language, m_log);
    m_log.Trace(L"[App] StringResources initialized");

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        m_log.Trace(L"[App] ERROR: GdiplusStartup failed");
        FailStartup(L"gdiplus-startup");
        return;
    }
    m_log.Trace(L"[App] GDI+ initialized");

    InitializeDeviceService();
    InitializeAppController();
    InitializeTray();
    InitializeAdaptiveResources();
    InitializeNotifications();
    SetupDeviceEvents();
    static_cast<void>(m_deviceService->Start());
    m_log.Trace(L"[App] Device watcher started");
    InitializeCommandLineControl();
    const auto startupConnections = m_appController->RestoreStartupConnections();

    if (m_notificationService && startupConnections == apc::app::AppController::StartupConnectionStatus::NoTargets) {
        try {
            m_notificationService->ShowAppStarted();
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[App] startup notification failed", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] startup notification failed", ex);
        } catch (...) {
            m_log.UnknownException(L"[App] startup notification failed");
        }
    }
    ScheduleDeviceVisualRefresh(false);

    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    util::crash::CheckAndPromptCrashReports(m_log.Path());
    m_log.Trace(L"[App] Initialization complete");
} catch (winrt::hresult_error const& ex) {
    m_log.Exception(L"[App] Initialization after MainWindow.Loaded failed", ex);
    FailStartup(L"main-window-loaded-hresult");
} catch (std::exception const& ex) {
    m_log.Exception(L"[App] Initialization after MainWindow.Loaded failed", ex);
    FailStartup(L"main-window-loaded-standard");
} catch (...) {
    m_log.UnknownException(L"[App] Initialization after MainWindow.Loaded failed");
    FailStartup(L"main-window-loaded-unknown");
}

void ApplicationHost::FailStartup(std::wstring_view stage) noexcept {
    m_log.Trace(L"[App] Startup aborted at stage={0}", stage);
    auto const settingsWindowClosed = PerformTeardown(SettingsShutdownMode::DiscardStartupFailure);
    auto const mainWindowClosed = CloseMainWindow(L"startup-failure");
    if (!settingsWindowClosed || !mainWindowClosed)
        TerminateAfterWindowCloseFailure(m_emergencyLog, L"startup-window-close-failure");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Initializers //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::InitializeTray() {
    m_log.Trace(L"[App] InitializeTray()");
    m_trayController = std::make_shared<TrayController>(m_log);
    auto weak = weak_from_this();
    m_trayController->Initialize(
        m_hwnd,
        m_mainWindow,
        m_appController,
        [weak] {
            if (auto self = weak.lock()) self->ExitApplication();
        },
        [weak] {
            if (auto self = weak.lock(); self && !self->m_exiting.load() && self->m_appController) {
                auto result = self->m_appController->ShowSettings();
                if (result.Succeeded()) static_cast<void>(self->m_settingsWindowPresenter.ShowHelp());
            }
        });
    m_log.Trace(L"[App] TrayController initialized");
}

void ApplicationHost::InitializeNotifications() {
    m_log.Trace(L"[App] InitializeNotifications()");
    m_notificationService = std::make_shared<NotificationService>(m_log);
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
                    if (self->m_exiting.load() || !self->m_appController) return;
                    if (auto selector = apc::app::DeviceSelector::ById(std::wstring_view(deviceId))) {
                        (void)self->m_appController->Reconnect(std::move(*selector),
                                                               apc::app::AppCommandContext::Detached());
                    }
                }
            }));
        }
    });
    const auto notificationsAvailable = m_notificationService->Initialize(
        winrt::hstring(_("AppName")), winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
    m_log.Trace(L"[App] Notifications available: {0}", notificationsAvailable);
}

void ApplicationHost::InitializeDeviceService() {
    m_log.Trace(L"[App] InitializeDeviceService()");
    m_deviceService =
        std::make_shared<apc::device::DeviceService>(apc::device::DeviceServiceDependencies{.Log = m_log});
    auto weak = weak_from_this();
    auto weakSettings = std::weak_ptr<SettingsStore>(m_settingsStore);
    m_startupTaskCoordinator = std::make_shared<StartupTaskCoordinator>(
        [weakSettings](bool enabled) {
            if (auto settings = weakSettings.lock()) (void)settings->SetStartWithWindows(enabled);
        },
        m_log);
    m_log.Trace(L"[App] DeviceService initialized");
}

apc::app::AppUiActionResult ApplicationHost::PresentDevicePicker(apc::app::DevicePickerOpenMode openMode,
                                                                 apc::app::AppCommandContext const& context) {
    auto weak = weak_from_this();
    apc::app::AppUiActionResult result;
    auto self = weak.lock();
    if (!self || !self->m_trayController) return result;

    auto tray = self->m_trayController;
    const auto openedGeneration = tray->DevicePickerOpenedGeneration();
    const auto wasVisible = tray->IsDevicePickerVisibleOrTransitioning();
    const auto uiResult = self->RunControlUiAction(
        [weak, openMode]() {
            auto self = weak.lock();
            return self && self->m_trayController &&
                   self->m_trayController->ShowDevicePicker(openMode == apc::app::DevicePickerOpenMode::ToggleIfOpen);
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
}

apc::app::AppUiActionResult ApplicationHost::PresentSettings(apc::app::AppCommandContext const& context) {
    auto weak = weak_from_this();
    apc::app::AppUiActionResult result;
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
}

apc::app::AppSnapshot::ResourceStatusSnapshot ApplicationHost::ResourceStatus() const {
    auto weak = weak_from_this();
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
}

std::uint64_t ApplicationHost::PickerOpenedGeneration() const {
    return m_trayController ? m_trayController->DevicePickerOpenedGeneration() : 0;
}

void ApplicationHost::InitializeAppController() {
    m_appController = std::make_shared<apc::app::AppController>(
        m_settingsStore, m_deviceService, weak_from_this(), m_startupTaskCoordinator);
    m_controlCommandAdapter = std::make_unique<apc::control::ControlCommandAdapter>(
        *m_appController, apc::control::ControlCommandAdapter::Options{[](std::string_view key) { return _(key); }});
    m_log.Trace(L"[App] AppController and control adapter initialized");
}

void ApplicationHost::InitializeCommandLineControl() {
    m_log.Trace(L"[App] InitializeCommandLineControl()");
    auto weak = weak_from_this();
    m_commandLineControlServer.Start([weak](apc::control::Request const& request,
                                            std::stop_token stopToken,
                                            std::uint64_t deadline) -> apc::control::Response {
        if (auto self = weak.lock(); self && self->m_controlCommandAdapter) {
            return self->m_controlCommandAdapter->Handle(request, stopToken, deadline);
        }
        return {apc::control::ExitCode::Unavailable, L""};
    });
    m_log.Trace(m_commandLineControlServer.IsRunning() ? L"[App] Command line control server started"
                                                       : L"[App] Command line control server retry scheduled");
}

void ApplicationHost::InitializeAdaptiveResources() noexcept {
    if (!m_trayController) return;
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
                    }
                    self->HandleResourcePressureSnapshot(value);
                }
            });

        if (!m_resourcePressureMonitor->Start()) {
            m_resourcePressureMonitor.reset();
            m_log.Trace(L"[App] Resource-pressure monitor unavailable; speculative preloading remains disabled");
        } else {
            m_powerSavingStatusNotification =
                RegisterPowerSettingNotification(m_hwnd, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (!m_powerSavingStatusNotification) {
                m_log.Trace(L"[App] Battery-saver notification registration unavailable; polling fallback remains "
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
        bool positiveAuthorizationCurrent;
        {
            std::scoped_lock authorizationLock(m_resourceAuthorizationMutex);
            positiveAuthorizationCurrent = IsPositiveResourceAuthorizationCurrent(
                m_lastResourcePressureSequence, m_latestConstrainedResourcePressureSequence);
        }
        // This capture admits the evaluation. Later pressure observations schedule
        // another UI evaluation; never hold the fence mutex across picker calls.
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
            m_log.Trace(L"[App] Adaptive resources reason={0} residency={1} background={2} action={3} memory={4} "
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
        AdaptiveResourceDiagnostics diagnostics = {
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
        {
            std::scoped_lock authorizationLock(m_resourceAuthorizationMutex);
            diagnostics.PositiveAuthorizationCurrent = IsPositiveResourceAuthorizationCurrent(
                m_lastResourcePressureSequence, m_latestConstrainedResourcePressureSequence);
            diagnostics.PreloadAllowed = input.PreloadAllowed && diagnostics.PositiveAuthorizationCurrent;
            m_adaptiveResourceDiagnostics = diagnostics;
        }
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

void ApplicationHost::HandlePowerSuspend() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleSuspend(
        [weak]() {
            if (auto self = weak.lock()) {
                if (!self->m_settingsStore || self->m_settingsStore->FlushNow(3)) return;
                self->m_log.Trace(L"[App] SettingsStore synchronous suspend flush failed after bounded attempts");
            }
        },
        [service = m_deviceService]() {
            return service ? service->SuspendForPowerTransition() : std::vector<std::wstring>{};
        });
}

void ApplicationHost::HandlePowerResume() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleResume(
        [service = m_deviceService]() {
            if (service) service->ResumeAfterPowerTransition();
        },
        [weak](std::vector<std::wstring> deviceIds,
               std::uint64_t generation,
               PowerTransitionCoordinator::ResumeReconnectCompleted completed) {
            auto finish = std::move(completed);

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
                self->m_deviceService->ResumeSuspendedSessions(deviceIds);
                for (auto const& deviceId : deviceIds) {
                    if (!deviceId.empty()) attemptedIds.push_back(deviceId);
                }
            });
            if (!accepted) finish({});
        });
}

bool ApplicationHost::RunOnUIThread(std::function<void()> work) noexcept {
    if (m_exiting.load() || !work) return false;

    bool hasThreadAccess = false;
    try {
        hasThreadAccess = m_dispatcherQueue && m_dispatcherQueue.HasThreadAccess();
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[App] Failed to query DispatcherQueue thread access", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[App] Failed to query DispatcherQueue thread access", ex);
    } catch (...) {
        m_log.UnknownException(L"[App] Failed to query DispatcherQueue thread access");
    }

    if (hasThreadAccess) {
        if (m_exiting.load()) return false;
        try {
            work();
            return true;
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[App] Inline UI work failed", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] Inline UI work failed", ex);
        } catch (...) {
            m_log.UnknownException(L"[App] Inline UI work failed");
        }
        return false;
    }

    try {
        if (m_dispatcherQueue) {
            auto weak = weak_from_this();
            auto dispatcherWork = work;
            if (m_dispatcherQueue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
                                             [weak, log = m_log, work = std::move(dispatcherWork)]() mutable noexcept {
                                                 try {
                                                     if (auto self = weak.lock(); self && !self->m_exiting.load()) {
                                                         work();
                                                     }
                                                 } catch (winrt::hresult_error const& ex) {
                                                     log.Exception(L"[App] UI-dispatched work failed", ex);
                                                 } catch (std::exception const& ex) {
                                                     log.Exception(L"[App] UI-dispatched work failed", ex);
                                                 } catch (...) {
                                                     log.UnknownException(L"[App] UI-dispatched work failed");
                                                 }
                                             })) {
                return true;
            }
        }
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[App] Dispatcher queue rejected UI work", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[App] Dispatcher queue rejected UI work", ex);
    } catch (...) {
        m_log.UnknownException(L"[App] Dispatcher queue rejected UI work");
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
    m_log.Trace(L"[App] ERROR: both DispatcherQueue and Win32 fallback rejected UI work");
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
            m_log.Exception(L"[App] Win32-fallback UI work failed", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] Win32-fallback UI work failed", ex);
        } catch (...) {
            m_log.UnknownException(L"[App] Win32-fallback UI work failed");
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
    if (m_exiting.load() || !m_trayController || !m_appController) {
        m_log.Trace(L"[App] RefreshTrayVisualState skipped reason={0} exiting={1} hasTrayController={2} "
                    L"hasAppController={3}",
                    reason,
                    m_exiting.load(),
                    m_trayController != nullptr,
                    m_appController != nullptr);
        return true;
    }
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        m_log.Trace(L"[App] RefreshTrayVisualState skipped reason={0} invalidHwnd hwnd=0x{1:X}",
                    reason,
                    reinterpret_cast<uintptr_t>(m_hwnd));
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (forceErrorWhenIdle) {
        m_trayErrorUntil = now + std::chrono::milliseconds(c_transientTrayErrorMs);
    }
    auto const snapshot = m_appController->Snapshot();
    if (!snapshot.IsRunning) return false;
    auto const& presentation = snapshot.Tray;
    const bool hasBusyOperations = presentation.HasBusyOperations;
    const bool hasConnections = !presentation.ConnectedDevices.empty();
    if (hasConnections) {
        m_trayErrorUntil = {};
        m_transientTrayErrorTooltip.clear();
    }
    const bool showTransientError = !hasConnections && !hasBusyOperations && now < m_trayErrorUntil;

    TrayIconState desiredState = TrayIconState::Idle;
    bool timersReady = true;
    if (hasBusyOperations) {
        desiredState = TrayIconState::Connecting;
        if (!m_connectingAnimationTimerActive) {
            if (SetTimer(m_hwnd, c_timerAnimation, 75, nullptr)) {
                m_connectingAnimationTimerActive = true;
            } else {
                timersReady = false;
                m_log.Trace(L"[App] Connecting animation timer unavailable: {0}", GetLastError());
            }
        }
        KillTimer(m_hwnd, c_timerTransientTrayError);
    } else if (hasConnections) {
        desiredState = TrayIconState::Connected;
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        KillTimer(m_hwnd, c_timerTransientTrayError);
    } else if (showTransientError) {
        desiredState = TrayIconState::Error;
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_trayErrorUntil - now);
        const auto delay = static_cast<UINT>(std::clamp<std::int64_t>(remaining.count(), 1, UINT_MAX));
        if (!SetTimer(m_hwnd, c_timerTransientTrayError, delay, nullptr)) {
            timersReady = false;
            m_log.Trace(L"[App] Transient tray error timer unavailable: {0}", GetLastError());
        }
    } else {
        KillTimer(m_hwnd, c_timerAnimation);
        m_connectingAnimationTimerActive = false;
        KillTimer(m_hwnd, c_timerTransientTrayError);
        m_trayErrorUntil = {};
    }

    m_log.Trace(L"[App] RefreshTrayVisualState reason={0} forceErrorWhenIdle={1} hasConnections={2} "
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
        m_trayController->UpdateTooltip(apc::tray::BuildTooltip(_("AppName"), _("Privacy_RedactedDevice"), snapshot));
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
                m_log.Trace(L"[App] Dispatcher rejected coalesced device visual refresh");
            } catch (winrt::hresult_error const& ex) {
                m_log.Exception(L"[App] Failed to enqueue coalesced device visual refresh", ex);
            } catch (std::exception const& ex) {
                m_log.Exception(L"[App] Failed to enqueue coalesced device visual refresh", ex);
            } catch (...) {
                m_log.UnknownException(L"[App] Failed to enqueue coalesced device visual refresh");
            }
        }

        if (m_hwnd && IsWindow(m_hwnd)) {
            if (PostMessageW(m_hwnd, c_messageDrainDeviceVisualRefresh, 0, 0)) return;
            if (SetTimer(m_hwnd, c_timerDeviceVisualRefreshRetry, 50, nullptr)) return;
        }

        if (ScheduleNativeDeviceVisualRefreshRetry(std::chrono::milliseconds(100))) return;

        if (!m_deviceVisualRefreshCoalescer.ScheduleFailed()) return;
    }

    m_log.Trace(L"[App] ERROR: no UI scheduling path accepted the device visual refresh");
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
        m_log.Exception(L"[App] Coalesced device visual refresh failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[App] Coalesced device visual refresh failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[App] Coalesced device visual refresh failed");
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
        m_log.Trace(L"[App] Device visual refresh retry scheduled in {0} ms", delay);
    } else {
        QueueDeviceVisualRefreshDrain();
    }
}

void ApplicationHost::SetupDeviceEvents() {
    auto weak = weak_from_this();
    auto observation =
        m_appController->SnapshotAndSubscribe([weak](apc::app::AppController::EventNotification const& event) {
            if (auto self = weak.lock()) {
                (void)self->RunOnUIThread([weak, event] {
                    if (auto current = weak.lock()) current->HandleAppEvent(event);
                });
            }
        });
    if (!observation.Updates) throw winrt::hresult_error(E_UNEXPECTED, L"Application observation unavailable");
    m_lastAppEventRevision = observation.Revision;
    m_appliedLanguage = observation.Snapshot.Settings.Language;
    m_appliedBackdrop = observation.Snapshot.Settings.UseSystemBackdropEffects;
    m_appEventSubscription = std::move(observation.Updates);
    ScheduleDeviceVisualRefresh(false, true);
}

void ApplicationHost::TeardownDeviceEvents() {
    m_appEventSubscription.Reset();
}

void ApplicationHost::HandleAppEvent(apc::app::AppController::EventNotification const& notification) {
    if (m_exiting.load() || !m_appController || notification.Revision <= m_lastAppEventRevision) return;
    m_lastAppEventRevision = notification.Revision;
    if (!m_appController->IsCurrent(notification)) return;
    std::visit(
        [this](auto const& event) {
            using T = std::decay_t<decltype(event)>;
            using namespace apc::app;
            if constexpr (std::is_same_v<T, DeviceConnectedEvent>) {
                OnDeviceConnected(winrt::hstring(event.Id.View()));
            } else if constexpr (std::is_same_v<T, DeviceDisconnectedEvent>) {
                OnDeviceDisconnected(winrt::hstring(event.Id.View()), event.NotifyUser);
            } else if constexpr (std::is_same_v<T, DeviceConnectionErrorEvent>) {
                using Reason = DeviceConnectionErrorEvent::Reason;
                auto const key = [&] {
                    switch (event.FailureReason) {
                        case Reason::TimedOut: return "RequestTimedOut";
                        case Reason::Denied: return "DeniedBySystem";
                        case Reason::ReconnectExhausted: return "AutoReconnectFailed";
                        case Reason::Unknown: return "UnknownError";
                    }
                    return "UnknownError";
                }();
                OnConnectionError(winrt::hstring(event.Id.View()), winrt::hstring(_(key)));
            } else if constexpr (std::is_same_v<T, AutoReconnectTriggeredEvent>) {
                OnAutoReconnectTriggered(winrt::hstring(event.Id.View()));
            } else if constexpr (std::is_same_v<T, AutoReconnectFailedEvent>) {
                OnAutoReconnectFailed(winrt::hstring(event.Id.View()));
            } else if constexpr (std::is_same_v<T, DeviceStatusChangedEvent>) {
                ScheduleDeviceVisualRefresh(event.State == DeviceConnectionState::Failed);
            } else if constexpr (std::is_same_v<T, DeviceInventoryChangedEvent>) {
                ScheduleDeviceVisualRefresh(false, true, false);
            } else if constexpr (std::is_same_v<T, DeviceActivityChangedEvent>) {
                ScheduleDeviceVisualRefresh(false);
            } else if constexpr (std::is_same_v<T, SettingsChangedEvent>) {
                if (m_appliedLanguage != event.Language) {
                    m_appliedLanguage = event.Language;
                    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), event.Language, m_log);
                    if (m_trayController) m_trayController->ApplyLanguage();
                }
                if (m_appliedBackdrop != event.UseSystemBackdropEffects) {
                    m_appliedBackdrop = event.UseSystemBackdropEffects;
                    if (m_trayController)
                        m_trayController->SetSystemBackdropEffectsEnabled(event.UseSystemBackdropEffects);
                }
                ScheduleDeviceVisualRefresh(false, true);
            }
        },
        notification.Event);
}

bool ApplicationHost::ShowSettingsWindow() {
    if (m_exiting.load()) return false;
    m_log.Trace(L"[App] ShowSettingsWindow()");
    auto placement =
        m_trayController ? m_trayController->GetSettingsWindowPlacement() : util::CalculateSettingsWindowPlacement();
    return m_settingsWindowPresenter.Show(m_appController, placement);
}

void ApplicationHost::ExitApplication() noexcept {
    m_log.Trace(L"[App] ExitApplication() started");
    auto const settingsWindowClosed = PerformTeardown(SettingsShutdownMode::Flush);
    auto const mainWindowClosed = CloseMainWindow(L"application-exit");
    if (!settingsWindowClosed || !mainWindowClosed)
        TerminateAfterWindowCloseFailure(m_emergencyLog, L"exit-window-close-failure");
    m_log.Trace(L"[App] ExitApplication() complete");
}

bool ApplicationHost::CloseMainWindow(std::wstring_view reason) noexcept {
    if (!m_mainWindow) return true;
    try {
        m_mainWindow.Close();
        return true;
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason), ex);
    } catch (std::exception const& ex) {
        m_log.Exception(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason), ex);
    } catch (...) {
        m_log.UnknownException(std::wstring(L"[App] Failed to close MainWindow: ") + std::wstring(reason));
    }
    return false;
}

/*//////// Device Event Handlers /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::OnDeviceConnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_deviceService) return;
    m_log.Trace(L"[App] OnDeviceConnected: {0}", std::wstring(id));

    if (!m_deviceService->IsDeviceConnected(std::wstring_view(id))) {
        return;
    }
    m_powerTransitionCoordinator.NotifyDeviceConnected(std::wstring_view(id));
    if (m_notificationService) {
        try {
            m_notificationService->ShowDeviceConnected(id, ResolveKnownDeviceName(id));
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[App] OnDeviceConnected notification ERROR", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] OnDeviceConnected notification ERROR", ex);
        } catch (...) {
            m_log.UnknownException(L"[App] OnDeviceConnected notification ERROR");
        }
    }

    ScheduleDeviceVisualRefresh(false);
}

void ApplicationHost::OnDeviceDisconnected(winrt::hstring const& id, bool notifyUser) {
    if (m_exiting.load()) return;
    m_log.Trace(L"[App] OnDeviceDisconnected: {0}", std::wstring(id));

    ScheduleDeviceVisualRefresh(false);
    if (!notifyUser || !m_notificationService) return;

    try {
        m_notificationService->ShowDeviceDisconnected(id, ResolveKnownDeviceName(id));
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[App] OnDeviceDisconnected notification ERROR", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[App] OnDeviceDisconnected notification ERROR", ex);
    } catch (...) {
        m_log.UnknownException(L"[App] OnDeviceDisconnected notification ERROR");
    }
}

void ApplicationHost::OnConnectionError(winrt::hstring const& id, winrt::hstring msg) {
    if (m_exiting.load()) return;
    m_log.Trace(L"[App] OnConnectionError: {0} - {1}", std::wstring(id), std::wstring(msg));
    m_transientTrayErrorTooltip = std::wstring(_("AppName")) + L"\n" + std::wstring(msg);
    ScheduleDeviceVisualRefresh(true);
}

void ApplicationHost::OnAutoReconnectTriggered(winrt::hstring const& id) {
    if (m_exiting.load()) return;
    m_log.Trace(L"[App] OnAutoReconnectTriggered: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    if (m_notificationService) {
        try {
            m_notificationService->ShowAutoReconnect(id, deviceName);
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[App] OnAutoReconnectTriggered notification ERROR", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] OnAutoReconnectTriggered notification ERROR", ex);
        }
    }
}

void ApplicationHost::OnAutoReconnectFailed(winrt::hstring const& id) {
    if (m_exiting.load()) return;
    m_log.Trace(L"[App] OnAutoReconnectFailed: {0}", std::wstring(id));

    winrt::hstring deviceName = ResolveKnownDeviceName(id);
    if (m_notificationService) {
        try {
            m_notificationService->ShowAutoReconnectFailed(id, deviceName);
        } catch (winrt::hresult_error const& ex) {
            m_log.Exception(L"[App] OnAutoReconnectFailed notification ERROR", ex);
        } catch (std::exception const& ex) {
            m_log.Exception(L"[App] OnAutoReconnectFailed notification ERROR", ex);
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

    // Release XAML owners before Windows/Restart Manager destroys the anchor and dispatcher.
    if (msg == WM_CLOSE) {
        host->ExitApplication();
        return 0;
    }
    if (msg == WM_ENDSESSION && wParam != FALSE) {
        static_cast<void>(host->PerformTeardown(SettingsShutdownMode::Flush));
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

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
        ThemeHelper::OnSettingChange(lParam, host->m_log);
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
