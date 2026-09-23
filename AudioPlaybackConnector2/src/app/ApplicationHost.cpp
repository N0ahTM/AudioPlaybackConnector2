#include <pch.h>
#include <windows.h>
#include <commctrl.h>
#include <winrt/Microsoft.UI.Windowing.h>

#include <app/ApplicationHost.hpp>
#include <type_traits>

#include <MainWindow/MainWindow.xaml.h>
#include <core/DeviceService.hpp>
#include <core/SettingsStore.hpp>
#include <core/StringResources.hpp>
#include <ui/TrayContextMenu.hpp>
#include <ui/TrayIcon.hpp>
#include <ui/XamlWindowInterop.hpp>
#include <util/CrashHandler.hpp>
#include <util/Logger.hpp>
#include <util/Util.hpp>

#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

constexpr int c_hiddenAnchorCoordinate = -32000;
constexpr auto c_mainWindowLoadedTimeout = std::chrono::seconds{15};

using OperationStatus = apc::app::AppActionStatus;

OperationStatus ToUiActionStatus(UiDispatcher::ActionResult result) noexcept {
    switch (result) {
        case UiDispatcher::ActionResult::Succeeded: return OperationStatus::Succeeded;
        case UiDispatcher::ActionResult::Failed: return OperationStatus::Failed;
        case UiDispatcher::ActionResult::Indeterminate: return OperationStatus::Indeterminate;
    }
    return OperationStatus::Failed;
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
    if (m_uiDispatcher) m_uiDispatcher->Stop();
    m_adaptiveResources->Stop();
    // Persist the window's final placement before closing application admission.
    auto const settingsWindowClosed = m_settingsWindowPresenter.Close();
    m_teardownWindowCloseSucceeded.store(settingsWindowClosed);
    if (m_appController) {
        m_appController->Shutdown();
    }

    StopMainWindowLoadedWatchdog();
    if (m_visualRefresh) m_visualRefresh->Stop();

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
    m_appEventSubscription.Reset();
    m_appController.reset();
    if (m_hwnd) {
        try {
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
    m_visualRefresh = std::make_unique<UiRefreshScheduler>(
        [dispatcher = m_dispatcherQueue](UiRefreshScheduler::Task task) {
            return dispatcher.TryEnqueue([task = std::move(task)] { task(); });
        },
        [weak = weak_from_this()](UiRefreshScheduler::Flags flags) {
            auto self = weak.lock();
            return self && self->RefreshDeviceVisuals(flags);
        },
        ~c_visualRefreshForceError,
        m_log);

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
    m_uiDispatcher = std::make_shared<UiDispatcher>(
        m_hwnd,
        [queue = m_dispatcherQueue](UiDispatcher::Task work) {
            return queue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
                                    [work = std::move(work)] { work(); });
        },
        m_log);
    m_log.Trace(L"[App] Window subclass installed");

    m_settingsStore = std::make_shared<SettingsStore>(std::filesystem::path{}, nullptr, nullptr, m_log);
    m_settingsStore->Load();
    m_log.Trace(L"[App] Settings loaded");

    const auto settingsSnapshot = m_settingsStore->Snapshot();
    m_strings->Initialize(GetModuleHandleW(nullptr), settingsSnapshot.Data.Language, m_log);
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
    m_adaptiveResources->Start(m_hwnd,
                               m_dispatcherQueue,
                               m_trayController,
                               [weak = std::weak_ptr<UiDispatcher>(m_uiDispatcher)](std::function<void()> work) {
                                   auto dispatcher = weak.lock();
                                   return dispatcher && dispatcher->Run(std::move(work));
                               });
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
    ScheduleDeviceVisualRefresh(c_visualRefreshRequested);

    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    util::crash::CheckAndPromptCrashReports(m_log.Path(), *m_strings);
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
    m_trayController = std::make_shared<TrayController>(m_log, m_strings);
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
    m_notificationService = std::make_shared<NotificationService>(m_log, m_strings, m_dispatcherQueue, m_appController);
    const auto notificationsAvailable =
        m_notificationService->Initialize(winrt::hstring(m_strings->Get("AppName")),
                                          winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
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
    const auto uiResult = self->m_uiDispatcher->RunAndWait(
        [weak, openMode]() {
            auto self = weak.lock();
            if (!self || !self->m_trayController ||
                !self->m_trayController->ShowDevicePicker(openMode == apc::app::DevicePickerOpenMode::ToggleIfOpen))
                return false;
            if (self->m_notificationService) self->m_notificationService->MaybeShowRatingPrompt();
            return true;
        },
        context.StopToken,
        context.Deadline);
    if (uiResult != UiDispatcher::ActionResult::Succeeded) {
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

    if (!wasVisible && tray->DevicePickerOpenedGeneration() == openedGeneration) {
        // A synchronous caller on the UI thread cannot wait for its own Opened event.
        if (m_dispatcherQueue.HasThreadAccess() ||
            !tray->WaitForDevicePickerOpened(openedGeneration, context.StopToken, context.Deadline)) {
            result.Status = OperationStatus::Indeterminate;
            return result;
        }
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
    const auto uiResult = self->m_uiDispatcher->RunAndWait(
        [weak]() {
            auto self = weak.lock();
            if (!self || !self->ShowSettingsWindow()) return false;
            if (self->m_notificationService) self->m_notificationService->MaybeShowRatingPrompt();
            return true;
        },
        context.StopToken,
        context.Deadline);
    // Preserve the gate's pre-dispatch versus in-flight distinction. The
    // context state is not sufficient once the UI callback may have run.
    result.Status = ToUiActionStatus(uiResult);
    return result;
}

apc::app::AppSnapshot::ResourceStatusSnapshot ApplicationHost::ResourceStatus() const {
    return m_adaptiveResources->Snapshot();
}

std::uint64_t ApplicationHost::PickerOpenedGeneration() const {
    return m_trayController ? m_trayController->DevicePickerOpenedGeneration() : 0;
}

void ApplicationHost::InitializeAppController() {
    m_appController = std::make_shared<apc::app::AppController>(
        m_settingsStore, m_deviceService, weak_from_this(), m_startupTaskCoordinator);
    m_controlCommandAdapter = std::make_unique<apc::control::ControlCommandAdapter>(
        *m_appController,
        apc::control::ControlCommandAdapter::Options{[strings = std::shared_ptr<StringResources const>(m_strings)](
                                                         std::string_view key) { return strings->Get(key); }});
    m_appController->RecordAppStart();
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
            auto accepted =
                self->m_uiDispatcher->Run([weak, generation, deviceIds = std::move(deviceIds), finish]() mutable {
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

void ApplicationHost::ScheduleDeviceVisualRefresh(UiRefreshScheduler::Flags refresh) {
    if (m_visualRefresh) m_visualRefresh->Request(refresh);
}

bool ApplicationHost::RefreshDeviceVisuals(UiRefreshScheduler::Flags flags) {
    if (m_exiting.load()) return false;
    // Rendering may pump window messages that tear down the host's tray ownership.
    auto tray = m_trayController;
    if (!tray) return true;
    if ((flags & c_visualRefreshInventoryChanged) != 0 && !tray->InvalidateDevicePickerInventory()) return false;
    return (flags & c_visualRefreshRequested) == 0 ||
           tray->RefreshVisualState((flags & c_visualRefreshForceError) != 0);
}

void ApplicationHost::SetupDeviceEvents() {
    auto weak = weak_from_this();
    auto observation =
        m_appController->SnapshotAndSubscribe([weak](apc::app::AppController::EventNotification const& event) {
            if (auto self = weak.lock()) {
                (void)self->m_uiDispatcher->Run([weak, event] {
                    if (auto current = weak.lock()) current->HandleAppEvent(event);
                });
            }
        });
    if (!observation.Updates) throw winrt::hresult_error(E_UNEXPECTED, L"Application observation unavailable");
    m_lastAppEventRevision = observation.Revision;
    m_appliedLanguage = observation.Snapshot.Settings.Language;
    m_appliedBackdrop = observation.Snapshot.Settings.UseSystemBackdropEffects;
    m_appEventSubscription = std::move(observation.Updates);
    ScheduleDeviceVisualRefresh(c_visualRefreshRequested | c_visualRefreshInventoryChanged);
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
                if (m_deviceService->IsDeviceConnected(event.Id.View())) {
                    m_powerTransitionCoordinator.NotifyDeviceConnected(event.Id.View());
                    ScheduleDeviceVisualRefresh(c_visualRefreshRequested);
                }
            } else if constexpr (std::is_same_v<T, DeviceDisconnectedEvent>) {
                ScheduleDeviceVisualRefresh(c_visualRefreshRequested);
            } else if constexpr (std::is_same_v<T, DeviceConnectionErrorEvent>) {
                if (m_trayController) m_trayController->SetConnectionError(event.FailureReason);
                ScheduleDeviceVisualRefresh(c_visualRefreshRequested | c_visualRefreshForceError);

            } else if constexpr (std::is_same_v<T, DeviceStatusChangedEvent>) {
                ScheduleDeviceVisualRefresh(event.State == DeviceConnectionState::Failed
                                                ? c_visualRefreshRequested | c_visualRefreshForceError
                                                : c_visualRefreshRequested);
            } else if constexpr (std::is_same_v<T, DeviceInventoryChangedEvent>) {
                ScheduleDeviceVisualRefresh(c_visualRefreshInventoryChanged);
            } else if constexpr (std::is_same_v<T, DeviceActivityChangedEvent>) {
                ScheduleDeviceVisualRefresh(c_visualRefreshRequested);
            } else if constexpr (std::is_same_v<T, SettingsChangedEvent>) {
                if (m_appliedLanguage != event.Language) {
                    m_appliedLanguage = event.Language;
                    m_strings->Initialize(GetModuleHandleW(nullptr), event.Language, m_log);
                    if (m_trayController) m_trayController->ApplyLanguage();
                    m_settingsWindowPresenter.ApplyLanguage(event.Language);
                }
                if (m_appliedBackdrop != event.UseSystemBackdropEffects) {
                    m_appliedBackdrop = event.UseSystemBackdropEffects;
                    if (m_trayController)
                        m_trayController->SetSystemBackdropEffectsEnabled(event.UseSystemBackdropEffects);
                }
                ScheduleDeviceVisualRefresh(c_visualRefreshRequested | c_visualRefreshInventoryChanged);
            }
        },
        notification.Event);
    if (m_notificationService) m_notificationService->HandleEvent(notification.Event);
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

    if (host->m_uiDispatcher && host->m_uiDispatcher->HandleMessage(msg)) return 0;

    if (msg == WM_SETTINGCHANGE) {
        if (host->m_trayController) host->m_trayController->OnSettingChange(lParam);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    if (host->m_adaptiveResources->HandleMessage(msg, wParam)) return 0;

    if (msg == WM_POWERBROADCAST) {
        switch (wParam) {
            case PBT_APMSUSPEND: host->HandlePowerSuspend(); return TRUE;
            case PBT_APMRESUMEAUTOMATIC:
            case PBT_APMRESUMESUSPEND: host->HandlePowerResume(); return TRUE;
            case PBT_POWERSETTINGCHANGE: return TRUE;
            default: break;
        }
    }

    if (msg == WM_TIMER && host->m_trayController && host->m_trayController->OnVisualTimer(wParam)) {
        host->ScheduleDeviceVisualRefresh(c_visualRefreshRequested);
        return 0;
    }

    if (host->m_taskbarCreatedMessage && msg == host->m_taskbarCreatedMessage) {
        if (host->m_trayController) {
            host->m_trayController->OnTaskbarCreated();
            host->ScheduleDeviceVisualRefresh(c_visualRefreshRequested);
        }
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
} catch (...) {
    OutputDebugStringW(L"[AudioPlaybackConnector2] Window subclass callback failed\n");
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
