#include <pch.h>

#include <app/ApplicationHost.hpp>

#include <MainWindow/MainWindow.xaml.h>
#include <app/AutoReconnectPlanner.hpp>
#include <app/StartupUpdateCoordinator.hpp>
#include <control/ControlTargetMatcher.hpp>
#include <core/DeviceDisplay.hpp>
#include <core/DeviceManager.hpp>
#include <core/Settings.hpp>
#include <core/SettingsLimits.hpp>
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

#include <cwctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

struct ControlDeviceInfo {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    bool Connected = false;
    bool Known = false;
};

constexpr DWORD c_controlDeviceRefreshTimeoutMs = 2500;
constexpr DWORD c_controlWaitPollMs = 50;
constexpr int c_hiddenAnchorCoordinate = -32000;
constexpr auto c_resourcePressureSnapshotMaximumAge = std::chrono::seconds{75};
constexpr auto c_mainWindowLoadedTimeout = std::chrono::seconds{15};

enum class ControlWaitResult { Completed, Cancelled, TimedOut, Failed };

template <typename TAsync>
ControlWaitResult WaitForControlAsync(TAsync const& operation, std::stop_token stopToken, std::uint64_t deadline) {
    std::shared_ptr<void> completed(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* handle) noexcept {
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
    });
    if (!completed) return ControlWaitResult::Failed;

    operation.Completed(
        [completed](auto const&, auto const&) noexcept { SetEvent(static_cast<HANDLE>(completed.get())); });
    while (true) {
        if (stopToken.stop_requested()) {
            operation.Cancel();
            return ControlWaitResult::Cancelled;
        }

        const auto remaining = apc::control::RemainingWait(deadline);
        if (remaining == 0) {
            operation.Cancel();
            return ControlWaitResult::TimedOut;
        }
        const auto waitResult =
            WaitForSingleObject(static_cast<HANDLE>(completed.get()), std::min<DWORD>(remaining, c_controlWaitPollMs));
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_TIMEOUT) {
            operation.Cancel();
            return ControlWaitResult::Failed;
        }
    }

    switch (operation.Status()) {
        case winrt::Windows::Foundation::AsyncStatus::Completed: return ControlWaitResult::Completed;
        case winrt::Windows::Foundation::AsyncStatus::Canceled: return ControlWaitResult::Cancelled;
        default: return ControlWaitResult::Failed;
    }
}

bool IsMutatingControlCommand(apc::control::CommandType command) noexcept {
    using apc::control::CommandType;
    switch (command) {
        case CommandType::Connect:
        case CommandType::Disconnect:
        case CommandType::Reconnect:
        case CommandType::ToggleLast:
        case CommandType::DisconnectAll:
        case CommandType::ReconnectAll:
        case CommandType::DefaultSet:
        case CommandType::DefaultClear:
        case CommandType::AliasSet:
        case CommandType::AliasClear: return true;
        default: return false;
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

std::optional<winrt::Windows::Devices::Enumeration::DeviceInformationCollection> TryRefreshControlDevices(
    std::shared_ptr<DeviceManager> const& manager, std::stop_token stopToken, std::uint64_t commandDeadline) {
    if (!manager) return std::nullopt;

    try {
        auto operation = manager->RefreshDevicesAsync();
        const auto localDeadline = apc::control::DeadlineAfter(c_controlDeviceRefreshTimeoutMs);
        const auto refreshDeadline = commandDeadline == 0 ? localDeadline : std::min(commandDeadline, localDeadline);
        if (WaitForControlAsync(operation, stopToken, refreshDeadline) != ControlWaitResult::Completed) {
            DebugTrace(L"[App] Control command device refresh timed out");
            return std::nullopt;
        }

        return operation.GetResults();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] Control command device refresh failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] Control command device refresh failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] Control command device refresh failed");
    }

    return std::nullopt;
}

std::wstring ToLowerInvariant(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (wchar_t ch : value) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return lowered;
}

bool EqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
    return ToLowerInvariant(lhs) == ToLowerInvariant(rhs);
}

bool ContainsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return false;
    return ToLowerInvariant(haystack).find(ToLowerInvariant(needle)) != std::wstring::npos;
}

std::wstring NormalizeHex(std::wstring_view value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t ch : value) {
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F')) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return normalized;
}

std::wstring DeviceLabel(ControlDeviceInfo const& device) {
    if (!device.Alias.empty()) return device.Alias;
    return device.Name.empty() ? device.Id : device.Name;
}

std::wstring DeviceDisplayLabel(ControlDeviceInfo const& device, bool redact) {
    return apc::display::DeviceNameOrId(device.Id, device.Name, device.Alias, redact);
}

std::wstring ResponseId(std::wstring_view id, bool redact) {
    return redact && !id.empty() ? std::wstring(_("Privacy_RedactedValue")) : std::wstring(id);
}

std::wstring ResponseName(ControlDeviceInfo const& device, bool redact) {
    return redact ? std::wstring() : device.Name;
}

std::wstring FormatResource(std::string_view key, std::wstring_view replacement) {
    return util::ReplacePlaceholders(_(key), replacement);
}

std::wstring FormatResource(std::string_view key, std::size_t value) {
    return FormatResource(key, std::to_wstring(value));
}

std::wstring FormatResource(std::string_view key, std::wstring_view first, std::wstring_view second) {
    auto result = util::ReplacePlaceholders(_(key), first);
    size_t pos = 0;
    while ((pos = result.find(L"{1}", pos)) != std::wstring::npos) {
        result.replace(pos, 3, second);
        pos += second.size();
    }
    return result;
}

void InsertDeviceJson(winrt::Windows::Data::Json::JsonObject& object, ControlDeviceInfo const& device, bool redact) {
    using winrt::Windows::Data::Json::JsonValue;
    object.Insert(L"id", JsonValue::CreateStringValue(winrt::hstring(ResponseId(device.Id, redact))));
    object.Insert(L"name", JsonValue::CreateStringValue(winrt::hstring(ResponseName(device, redact))));
    object.Insert(L"alias", JsonValue::CreateStringValue(winrt::hstring(device.Alias)));
    object.Insert(L"displayName", JsonValue::CreateStringValue(winrt::hstring(DeviceDisplayLabel(device, redact))));
    object.Insert(L"connected", JsonValue::CreateBooleanValue(device.Connected));
    object.Insert(L"known", JsonValue::CreateBooleanValue(device.Known));
    object.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redact));
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

bool ApplicationHost::PerformTeardown(bool saveSettings) noexcept {
    if (m_exiting.exchange(true)) return m_teardownWindowCloseSucceeded.load();

    StopMainWindowLoadedWatchdog();
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
    m_settingsSaver.Cancel();
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
    auto const settingsWindowClosed = m_settingsWindowPresenter.Close(false);
    m_teardownWindowCloseSucceeded.store(settingsWindowClosed);
    if (m_updateCoordinator) {
        m_updateCoordinator->Shutdown();
    }
    m_commandLineControlServer.Stop();
    TeardownDeviceEvents();
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
    if (saveSettings) {
        static_cast<void>(m_settingsSaver.FlushNow(3));
    }
    if (m_deviceManager) {
        m_deviceManager->ShutdownForProcessExit();
        m_deviceManager.reset();
    }
    m_notificationService.reset();
    m_updateCoordinator.reset();
    m_trayController.reset();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
    return settingsWindowClosed;
}

void ApplicationHost::Shutdown() noexcept {
    static_cast<void>(PerformTeardown(/*saveSettings=*/true));
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

    m_settings = std::make_shared<::Settings>();
    m_settings->Load(GetModuleHandleW(nullptr));
    m_settingsSaver.Initialize(m_settings, m_hwnd);
    DebugTrace(L"[App] Settings loaded");

    {
        auto locked = m_settings->LockSharedData();
        StringResources::Instance().Initialize(GetModuleHandleW(nullptr), locked->Language);
    }
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

    InitializeDeviceManager();
    InitializeTray();
    InitializeAdaptiveResources();
    InitializeNotifications();
    SetupDeviceEvents();
    {
        auto locked = m_settings->LockSharedData();
        m_deviceManager->SetIncomingConnectionsEnabled(locked->AllowIncomingConnections);
    }
    m_deviceManager->StartDeviceWatcher();
    DebugTrace(L"[App] Device watcher started");
    InitializeCommandLineControl();
    bool willAutoReconnect = false;
    {
        auto locked = m_settings->LockSharedData();
        willAutoReconnect = AutoReconnectPlanner::HasReconnectTargets(*locked);
    }

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
    auto const settingsWindowClosed = PerformTeardown(/*saveSettings=*/false);
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
    m_trayController->SetDeviceManager(m_deviceManager);
    m_trayController->SetSettings(m_settings);
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
                self->m_settings) {
                auto locked = self->m_settings->LockSharedData();
                self->m_trayController->SetSystemBackdropEffectsEnabled(locked->UseSystemBackdropEffects);
                return;
            }
            self->ScheduleDeviceVisualRefresh(false);
        });
    }
    m_trayController->SetCallbacks(
        [weak]() {
            if (auto self = weak.lock()) (void)self->ShowSettingsWindow();
        },
        [weak]() {
            if (auto self = weak.lock()) self->ExitApplication();
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock(); self && self->m_deviceManager) self->m_deviceManager->ConnectDetached(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock(); self && self->m_deviceManager) self->m_deviceManager->Disconnect(id);
        },
        [weak](winrt::hstring id) {
            if (auto self = weak.lock(); self && self->m_deviceManager) self->m_deviceManager->ReconnectDetached(id);
        },
        [weak]() {
            if (auto self = weak.lock()) self->ToggleLastConnectedDeviceFromTray();
        },
        [weak]() {
            if (auto self = weak.lock(); self && self->m_deviceManager) self->m_deviceManager->DisconnectAll();
        },
        [weak]() {
            if (auto self = weak.lock(); self && self->m_deviceManager) self->m_deviceManager->ReconnectAll();
        });
    DebugTrace(L"[App] TrayController initialized");
}

void ApplicationHost::InitializeNotifications() {
    DebugTrace(L"[App] InitializeNotifications()");
    m_notificationService = std::make_shared<NotificationService>();
    auto weak = weak_from_this();
    m_notificationService->SetShouldShowNotificationCallback([weak]() -> bool {
        if (auto self = weak.lock()) {
            if (!self->m_settings) return true;
            auto locked = self->m_settings->LockSharedData();
            return locked->ShowNotifications;
        }
        return true;
    });
    m_notificationService->SetReconnectCallback([weak](winrt::hstring deviceId) {
        if (auto self = weak.lock()) {
            static_cast<void>(self->RunOnUIThread([weak, deviceId = std::move(deviceId)]() mutable {
                if (auto self = weak.lock()) {
                    if (self->m_exiting.load() || !self->m_deviceManager) return;
                    self->m_deviceManager->ReconnectDetached(deviceId);
                }
            }));
        }
    });
    const auto notificationsAvailable = m_notificationService->Initialize(
        winrt::hstring(_("AppName")), winrt::Windows::Foundation::Uri(L"ms-appx:///Images/Square44x44Logo.png"));
    DebugTrace(L"[App] Notifications available: {0}", notificationsAvailable);
}

void ApplicationHost::InitializeDeviceManager() {
    DebugTrace(L"[App] InitializeDeviceManager()");
    m_deviceManager = std::make_shared<DeviceManager>();
    auto weak = weak_from_this();
    m_settingsController = std::make_shared<SettingsController>(m_settings, m_deviceManager, [weak]() {
        if (auto self = weak.lock(); self && !self->m_exiting.load()) self->m_settingsSaver.RequestSave();
    });
    m_deviceManager->SetReconnectOnConnectionLossPredicate([weak](auto id) {
        auto self = weak.lock();
        if (!self || self->m_exiting.load() || !self->m_settings) return false;
        auto locked = self->m_settings->LockSharedData();
        if (locked->GlobalReconnectOnConnectionLoss) return true;
        return std::ranges::any_of(locked->Devices,
                                   [&](const auto& d) { return d.Id == id && d.ReconnectOnConnectionLoss; });
    });
    DebugTrace(L"[App] DeviceManager initialized");
}

void ApplicationHost::InitializeCommandLineControl() {
    DebugTrace(L"[App] InitializeCommandLineControl()");
    auto weak = weak_from_this();
    m_commandLineControlServer.Start([weak](apc::control::Request const& request,
                                            std::stop_token stopToken,
                                            std::uint64_t deadline) -> apc::control::Response {
        if (auto self = weak.lock()) {
            return self->HandleControlCommand(request, stopToken, deadline);
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
    if (!m_settings) return id;
    auto locked = m_settings->LockSharedData();
    auto it = std::ranges::find_if(locked->Devices, [&](const auto& device) { return device.Id == id; });
    if (it != locked->Devices.end()) {
        if (!it->Alias.empty()) return winrt::hstring(it->Alias);
        if (locked->PrivacyModeEnabled) return winrt::hstring(_("Privacy_RedactedDevice"));
        if (!it->Name.empty()) return winrt::hstring(it->Name);
    }
    return locked->PrivacyModeEnabled ? winrt::hstring(_("Privacy_RedactedDevice")) : id;
}

std::optional<std::wstring> ApplicationHost::ResolveDefaultDeviceId() const {
    if (!m_settings) return std::nullopt;

    auto locked = m_settings->LockSharedData();
    if (locked->DefaultDevice == DefaultDeviceMode::SpecificDevice && !locked->DefaultDeviceId.empty()) {
        return locked->DefaultDeviceId;
    }
    if (!locked->LastConnectedIds.empty()) {
        return locked->LastConnectedIds.front();
    }
    return std::nullopt;
}

void ApplicationHost::ToggleLastConnectedDeviceFromTray() {
    if (m_exiting.load() || !m_settings || !m_deviceManager) return;

    if (m_deviceManager->HasBusyOperations()) {
        DebugTrace(L"[App] Tray double-click ignored: device operation in progress");
        return;
    }

    auto targetId = ResolveDefaultDeviceId();
    if (!targetId) {
        DebugTrace(L"[App] Tray double-click ignored: no default or last connected device");
        return;
    }

    if (targetId->empty()) return;

    auto id = winrt::hstring(*targetId);

    if (m_deviceManager->IsDeviceBusy(id)) {
        DebugTrace(L"[App] Tray double-click ignored: device busy: {0}", *targetId);
        return;
    }

    if (m_deviceManager->IsDeviceConnected(id)) {
        DebugTrace(L"[App] Tray double-click: disconnecting {0}", *targetId);
        m_deviceManager->Disconnect(id);
    } else {
        DebugTrace(L"[App] Tray double-click: connecting {0}", *targetId);
        m_deviceManager->ConnectDetached(id);
    }
    ScheduleDeviceVisualRefresh(false);
}

void ApplicationHost::TryAutoReconnect() {
    if (m_exiting.load() || !m_settings || !m_deviceManager) return;

    DebugTrace(L"[App] TryAutoReconnect()");
    std::vector<std::wstring> reconnectIds;
    {
        auto locked = m_settings->LockSharedData();
        reconnectIds = AutoReconnectPlanner::BuildReconnectPlan(*locked);
    }

    for (const auto& id : reconnectIds) {
        DebugTrace(L"[App] Auto-reconnecting to: {0}", id);
        m_deviceManager->ConnectDetached(winrt::hstring(id));
    }
}

void ApplicationHost::HandlePowerSuspend() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleSuspend(
        [weak]() {
            if (auto self = weak.lock()) {
                static_cast<void>(self->m_settingsSaver.FlushNow());
            }
        },
        m_deviceManager);
}

void ApplicationHost::HandlePowerResume() {
    auto weak = weak_from_this();
    m_powerTransitionCoordinator.HandleResume(
        m_deviceManager,
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
                if (!self || !self->m_deviceManager ||
                    !self->m_powerTransitionCoordinator.IsResumeReconnectGenerationCurrent(generation)) {
                    return;
                }
                for (auto const& deviceId : deviceIds) {
                    if (deviceId.empty() || self->m_deviceManager->IsDeviceConnected(winrt::hstring(deviceId)) ||
                        self->m_deviceManager->IsDeviceBusy(winrt::hstring(deviceId))) {
                        continue;
                    }
                    self->m_deviceManager->ConnectDetached(winrt::hstring(deviceId));
                    attemptedIds.push_back(deviceId);
                }
            });
            if (!accepted) finish({});
        });
}

winrt::fire_and_forget ApplicationHost::CheckForUpdatesOnStartupAsync() {
    try {
        auto lifetime = shared_from_this();
        auto settings = m_settings;
        auto notificationService = m_notificationService;
        auto updateCoordinator = m_updateCoordinator;
        if (m_exiting.load() || !settings || !notificationService || !updateCoordinator) co_return;
        co_await StartupUpdateCoordinator::CheckForUpdatesAsync(
            *settings, notificationService, updateCoordinator, m_exiting);
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

ApplicationHost::ControlUiActionResult
ApplicationHost::RunControlUiAction(std::function<bool()> work, std::stop_token stopToken, std::uint64_t deadline) {
    if (m_exiting.load() || !m_dispatcherQueue || stopToken.stop_requested()) {
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
        if (stopToken.stop_requested() || m_exiting.load()) {
            return state->Gate.CancelOrClassify();
        }
        const auto remaining = apc::control::RemainingWait(deadline);
        if (remaining == 0) return state->Gate.CancelOrClassify();
        const auto waitResult = WaitForSingleObject(state->Completed.get(), std::min<DWORD>(remaining, 100));
        if (waitResult == WAIT_OBJECT_0) {
            return state->Gate.CurrentResult();
        }
        if (waitResult != WAIT_TIMEOUT) return state->Gate.CancelOrClassify();
    }
}

bool ApplicationHost::RefreshTrayVisualState(bool forceErrorWhenIdle, std::wstring_view reason) {
    if (m_exiting.load() || !m_trayController || !m_deviceManager) {
        DebugTrace(L"[App] RefreshTrayVisualState skipped reason={0} exiting={1} hasTrayController={2} "
                   L"hasDeviceManager={3}",
                   reason,
                   m_exiting.load(),
                   m_trayController != nullptr,
                   m_deviceManager != nullptr);
        return true;
    }
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        DebugTrace(L"[App] RefreshTrayVisualState skipped reason={0} invalidHwnd hwnd=0x{1:X}",
                   reason,
                   reinterpret_cast<uintptr_t>(m_hwnd));
        return true;
    }

    auto const presentation = m_deviceManager->GetTrayPresentationSnapshot();
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
        if (auto self = weak.lock()) self->OnDeviceConnected(id);
    };
    callbacks.DeviceDisconnected = [weak](auto const& id) {
        if (auto self = weak.lock()) self->OnDeviceDisconnected(id);
    };
    callbacks.ConnectionError = [weak](auto const& id, auto const& msg) {
        if (auto self = weak.lock()) self->OnConnectionError(id, msg);
    };
    callbacks.AutoReconnectTriggered = [weak](auto const& id) {
        if (auto self = weak.lock()) self->OnAutoReconnectTriggered(id);
    };
    callbacks.AutoReconnectFailed = [weak](auto const& id) {
        if (auto self = weak.lock()) self->OnAutoReconnectFailed(id);
    };
    callbacks.DeviceStatusChanged = [weak](auto const&, auto const&, DeviceStatusKind statusKind) {
        auto self = weak.lock();
        if (!self) return;
        if (self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
        if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
        self->ScheduleDeviceVisualRefresh(statusKind == DeviceStatusKind::Error);
    };
    callbacks.DeviceActivityChanged = [weak]() {
        auto self = weak.lock();
        if (!self || self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
        if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
        self->ScheduleDeviceVisualRefresh(false);
    };
    callbacks.DeviceInventoryChanged = [weak]() {
        auto self = weak.lock();
        if (!self || self->m_exiting.load() || !self->m_trayController || !self->m_deviceManager) return;
        if (!self->m_hwnd || !IsWindow(self->m_hwnd)) return;
        self->ScheduleDeviceVisualRefresh(false, true, false);
    };

    m_deviceEventRouter.Attach(
        m_deviceManager,
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

bool ApplicationHost::ShowSettingsWindow() {
    if (m_exiting.load()) return false;
    DebugTrace(L"[App] ShowSettingsWindow()");
    auto weak = weak_from_this();
    return m_settingsWindowPresenter.Show(m_settingsController, m_trayController, m_updateCoordinator, [weak]() {
        auto self = weak.lock();
        if (self) static_cast<void>(self->m_settingsSaver.FlushNow());
    });
}

void ApplicationHost::ExitApplication() noexcept {
    DebugTrace(L"[App] ExitApplication() started");
    auto const settingsWindowClosed = PerformTeardown(/*saveSettings=*/true);
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

apc::control::Response ApplicationHost::HandleControlCommand(apc::control::Request const& request,
                                                             std::stop_token stopToken,
                                                             std::uint64_t deadline) {
    using apc::control::CommandFlagJson;
    using apc::control::CommandFlagRaw;
    using apc::control::CommandType;
    using apc::control::ExitCode;
    using apc::control::Response;
    using apc::control::TargetKind;
    using winrt::Windows::Data::Json::JsonArray;
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValue;

    const bool wantsJson = (request.Flags & CommandFlagJson) != 0;
    const bool wantsRaw = (request.Flags & CommandFlagRaw) != 0;

    auto makeResponse = [wantsJson](ExitCode code, std::wstring message) -> Response {
        if (!wantsJson) return {code, std::move(message)};

        JsonObject root;
        root.Insert(L"ok", JsonValue::CreateBooleanValue(code == ExitCode::Success));
        root.Insert(L"exitCode", JsonValue::CreateNumberValue(static_cast<double>(code)));
        root.Insert(L"message", JsonValue::CreateStringValue(winrt::hstring(message)));
        return {code, std::wstring(root.Stringify())};
    };

    if (stopToken.stop_requested() || apc::control::RemainingWait(deadline) == 0 || m_exiting.load() ||
        !m_deviceManager || !m_settings) {
        return makeResponse(ExitCode::Unavailable, _("Command_NotReady"));
    }

    const bool redactOutput = [&]() {
        auto locked = m_settings->LockSharedData();
        return locked->PrivacyModeEnabled && !wantsRaw;
    }();

    auto makeOperationResponse = [wantsJson, redactOutput](ExitCode code,
                                                           std::wstring_view action,
                                                           std::wstring_view id,
                                                           std::wstring_view name,
                                                           std::wstring message) -> Response {
        if (!wantsJson) return {code, std::move(message)};

        JsonObject root;
        root.Insert(L"ok", JsonValue::CreateBooleanValue(code == ExitCode::Success));
        root.Insert(L"exitCode", JsonValue::CreateNumberValue(static_cast<double>(code)));
        root.Insert(L"action", JsonValue::CreateStringValue(winrt::hstring(action)));
        root.Insert(L"id", JsonValue::CreateStringValue(winrt::hstring(ResponseId(id, redactOutput))));
        root.Insert(L"name", JsonValue::CreateStringValue(winrt::hstring(name)));
        root.Insert(L"displayName", JsonValue::CreateStringValue(winrt::hstring(name)));
        root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redactOutput));
        root.Insert(L"message", JsonValue::CreateStringValue(winrt::hstring(message)));
        return {code, std::wstring(root.Stringify())};
    };

    std::unique_lock mutationLock(m_controlMutationMutex, std::defer_lock);
    if (IsMutatingControlCommand(request.Command)) {
        if (!mutationLock.try_lock()) {
            DebugTrace(L"[App] Control command rejected as busy: command={0}", static_cast<uint32_t>(request.Command));
            return makeResponse(ExitCode::Busy, _("Command_Busy"));
        }
    }

    auto buildDevices = [this, stopToken, deadline](bool refreshLiveDevices) {
        std::unordered_map<std::wstring, ControlDeviceInfo> byId;
        auto upsert = [&byId](std::wstring id, std::wstring name, std::wstring alias, bool connected, bool known) {
            if (id.empty()) return;
            auto& entry = byId[id];
            entry.Id = std::move(id);
            if (!name.empty()) {
                entry.Name = std::move(name);
            }
            if (!alias.empty()) {
                entry.Alias = std::move(alias);
            }
            entry.Connected = entry.Connected || connected;
            entry.Known = entry.Known || known;
        };

        if (refreshLiveDevices) {
            if (auto devices = TryRefreshControlDevices(m_deviceManager, stopToken, deadline)) {
                for (auto const& device : *devices) {
                    upsert(std::wstring(device.Id()), std::wstring(device.Name()), L"", false, false);
                }
            }
        }

        for (auto const& connection : m_deviceManager->GetConnectedDevices()) {
            upsert(connection.Id, connection.Name, L"", true, true);
        }

        {
            auto locked = m_settings->LockSharedData();
            for (auto const& device : locked->Devices) {
                upsert(device.Id, device.Name, device.Alias, false, true);
            }
        }

        std::vector<ControlDeviceInfo> devices;
        devices.reserve(byId.size());
        for (auto& [id, info] : byId) {
            devices.push_back(std::move(info));
        }
        std::ranges::sort(devices, [](auto const& lhs, auto const& rhs) {
            auto lhsLabel = ToLowerInvariant(DeviceLabel(lhs));
            auto rhsLabel = ToLowerInvariant(DeviceLabel(rhs));
            if (lhsLabel != rhsLabel) return lhsLabel < rhsLabel;
            return ToLowerInvariant(lhs.Id) < ToLowerInvariant(rhs.Id);
        });
        return devices;
    };

    auto deviceJson = [redactOutput](ControlDeviceInfo const& device) {
        JsonObject object;
        InsertDeviceJson(object, device, redactOutput);
        return object;
    };

    auto listDevices = [&]() -> Response {
        auto devices = buildDevices(true);
        if (wantsJson) {
            JsonObject root;
            JsonArray deviceArray;
            for (auto const& device : devices) {
                deviceArray.Append(deviceJson(device));
            }
            root.Insert(L"devices", deviceArray);
            return {ExitCode::Success, std::wstring(root.Stringify())};
        }

        if (devices.empty()) {
            return {ExitCode::Success, _("Command_List_NoDevices") + L"\n"};
        }

        std::wstringstream output;
        output << _("Command_List_Header") << L"\n";
        for (auto const& device : devices) {
            output << L"- " << DeviceDisplayLabel(device, redactOutput);
            if (device.Connected) {
                output << L" (" << _("Command_ConnectedSuffix") << L")";
            }
            output << L"\n  ID: " << ResponseId(device.Id, redactOutput) << L"\n";
        }
        return {ExitCode::Success, output.str()};
    };

    auto status = [&]() -> Response {
        auto devices = buildDevices(false);
        std::vector<ControlDeviceInfo> connected;
        std::ranges::copy_if(
            devices, std::back_inserter(connected), [](auto const& device) { return device.Connected; });

        if (wantsJson) {
            JsonObject root;
            JsonArray connectedArray;
            for (auto const& device : connected) {
                connectedArray.Append(deviceJson(device));
            }
            root.Insert(L"running", JsonValue::CreateBooleanValue(true));
            root.Insert(L"connectedCount", JsonValue::CreateNumberValue(static_cast<double>(connected.size())));
            root.Insert(L"connectedDevices", connectedArray);
            return {ExitCode::Success, std::wstring(root.Stringify())};
        }

        std::wstringstream output;
        output << _("Command_Status_Running") << L"\n";
        output << FormatResource("Command_Status_Connections", connected.size()) << L"\n";
        for (auto const& device : connected) {
            output << L"- " << DeviceDisplayLabel(device, redactOutput) << L"\n  ID: "
                   << ResponseId(device.Id, redactOutput) << L"\n";
        }
        return {ExitCode::Success, output.str()};
    };

    struct TargetResolution {
        ExitCode Code = ExitCode::Success;
        std::wstring Id;
        std::wstring Name;
        std::wstring RawName;
        std::wstring Message;
        bool Exists = false;
    };

    auto targetFromId = [&](std::wstring id, std::vector<ControlDeviceInfo> const& devices) -> TargetResolution {
        for (auto const& device : devices) {
            if (EqualsIgnoreCase(device.Id, id)) {
                return {.Id = device.Id,
                        .Name = DeviceDisplayLabel(device, redactOutput),
                        .RawName = device.Name,
                        .Exists = true};
            }
        }
        auto name = redactOutput ? std::wstring(_("Privacy_RedactedDevice")) : id;
        return {.Id = std::move(id), .Name = std::move(name)};
    };

    auto matchOne = [&](std::vector<ControlDeviceInfo> matches, std::wstring_view query) -> TargetResolution {
        std::ranges::sort(matches, [](auto const& lhs, auto const& rhs) { return lhs.Id < rhs.Id; });
        auto last = std::ranges::unique(matches, [](auto const& lhs, auto const& rhs) { return lhs.Id == rhs.Id; });
        matches.erase(last.begin(), last.end());

        if (matches.empty()) {
            return {ExitCode::NotFound, L"", L"", L"", FormatResource("Command_TargetNotFound", query)};
        }
        if (matches.size() > 1) {
            return {ExitCode::Ambiguous, L"", L"", L"", FormatResource("Command_TargetAmbiguous", query)};
        }
        return {.Id = matches.front().Id,
                .Name = DeviceDisplayLabel(matches.front(), redactOutput),
                .RawName = matches.front().Name,
                .Exists = true};
    };

    auto resolveTarget = [&](apc::control::Request const& commandRequest) -> TargetResolution {
        const bool refreshLiveDevices = commandRequest.Target == TargetKind::Name ||
                                        commandRequest.Target == TargetKind::Mac ||
                                        commandRequest.Target == TargetKind::Auto;
        auto devices = buildDevices(refreshLiveDevices);
        if (commandRequest.Target == TargetKind::Default) {
            auto id = ResolveDefaultDeviceId();
            if (!id) {
                return {ExitCode::NotFound, L"", L"", L"", _("Command_DefaultTargetMissing")};
            }
            return targetFromId(std::move(*id), devices);
        }

        if (commandRequest.Target == TargetKind::Last) {
            std::wstring id;
            {
                auto locked = m_settings->LockSharedData();
                if (locked->LastConnectedIds.empty()) {
                    return {ExitCode::NotFound, L"", L"", L"", _("Command_LastTargetMissing")};
                }
                id = locked->LastConnectedIds.front();
            }
            return targetFromId(std::move(id), devices);
        }

        if (commandRequest.Payload.empty()) {
            return {ExitCode::InvalidRequest, L"", L"", L"", _("Command_TargetRequired")};
        }

        if (commandRequest.Target == TargetKind::Id) {
            return targetFromId(commandRequest.Payload, devices);
        }

        std::vector<ControlDeviceInfo> matches;
        if (commandRequest.Target == TargetKind::Auto) {
            std::vector<apc::control::TargetCandidateView> candidates;
            candidates.reserve(devices.size());
            for (auto const& device : devices)
                candidates.push_back({device.Id, device.Name, device.Alias});
            const auto selection = apc::control::FindAutoTargetMatches(candidates, commandRequest.Payload);
            matches.reserve(selection.Indices.size());
            for (auto index : selection.Indices)
                matches.push_back(devices[index]);
            return matchOne(std::move(matches), commandRequest.Payload);
        }

        if (commandRequest.Target == TargetKind::Mac) {
            const auto queryHex = NormalizeHex(commandRequest.Payload);
            if (queryHex.size() >= 6) {
                matches.clear();
                for (auto const& device : devices) {
                    if (NormalizeHex(device.Id).find(queryHex) != std::wstring::npos) {
                        matches.push_back(device);
                    }
                }
                return matchOne(std::move(matches), commandRequest.Payload);
            }
            return matchOne({}, commandRequest.Payload);
        }

        if (commandRequest.Target == TargetKind::Alias) {
            matches.clear();
            for (auto const& device : devices) {
                if (EqualsIgnoreCase(device.Alias, commandRequest.Payload)) {
                    matches.push_back(device);
                }
            }
            if (!matches.empty()) return matchOne(std::move(matches), commandRequest.Payload);

            matches.clear();
            for (auto const& device : devices) {
                if (ContainsIgnoreCase(device.Alias, commandRequest.Payload)) {
                    matches.push_back(device);
                }
            }
            return matchOne(std::move(matches), commandRequest.Payload);
        }

        if (commandRequest.Target == TargetKind::Name) {
            matches.clear();
            for (auto const& device : devices) {
                if (EqualsIgnoreCase(device.Name, commandRequest.Payload)) {
                    matches.push_back(device);
                }
            }
            if (!matches.empty()) return matchOne(std::move(matches), commandRequest.Payload);

            matches.clear();
            for (auto const& device : devices) {
                if (ContainsIgnoreCase(device.Name, commandRequest.Payload)) {
                    matches.push_back(device);
                }
            }
            return matchOne(std::move(matches), commandRequest.Payload);
        }

        return {ExitCode::InvalidRequest, L"", L"", L"", _("Command_TargetRequired")};
    };

    auto connectTarget = [&](TargetResolution const& target, std::wstring_view action) -> Response {
        if (m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
            return makeOperationResponse(ExitCode::Success,
                                         action,
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_DeviceAlreadyConnected", target.Name));
        }

        auto connectOperation = m_deviceManager->ConnectAsync(winrt::hstring(target.Id));
        const auto waitResult = WaitForControlAsync(connectOperation, stopToken, deadline);
        if (waitResult != ControlWaitResult::Completed) {
            return makeOperationResponse(ExitCode::Indeterminate,
                                         action,
                                         target.Id,
                                         target.Name,
                                         waitResult == ControlWaitResult::Cancelled
                                             ? std::wstring(_("Command_NotReady"))
                                             : FormatResource("Command_ConnectFailed", target.Name));
        }
        if (!m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
            return makeOperationResponse(ExitCode::OperationFailed,
                                         action,
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_ConnectFailed", target.Name));
        }

        return makeOperationResponse(
            ExitCode::Success, action, target.Id, target.Name, FormatResource("Command_ConnectSucceeded", target.Name));
    };

    auto disconnectTarget = [&](TargetResolution const& target, std::wstring_view action) -> Response {
        if (!m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
            return makeOperationResponse(ExitCode::Success,
                                         action,
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_DeviceAlreadyDisconnected", target.Name));
        }

        m_deviceManager->Disconnect(winrt::hstring(target.Id));
        if (m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
            return makeOperationResponse(ExitCode::OperationFailed,
                                         action,
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_DisconnectFailed", target.Name));
        }

        return makeOperationResponse(ExitCode::Success,
                                     action,
                                     target.Id,
                                     target.Name,
                                     FormatResource("Command_DisconnectSucceeded", target.Name));
    };

    auto reconnectTarget = [&](TargetResolution const& target) -> Response {
        auto reconnectOperation = m_deviceManager->ReconnectAsync(winrt::hstring(target.Id));
        const auto waitResult = WaitForControlAsync(reconnectOperation, stopToken, deadline);
        if (waitResult != ControlWaitResult::Completed) {
            return makeOperationResponse(ExitCode::Indeterminate,
                                         L"reconnect",
                                         target.Id,
                                         target.Name,
                                         waitResult == ControlWaitResult::Cancelled
                                             ? std::wstring(_("Command_NotReady"))
                                             : FormatResource("Command_ReconnectFailed", target.Name));
        }
        if (!m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
            return makeOperationResponse(ExitCode::OperationFailed,
                                         L"reconnect",
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_ReconnectFailed", target.Name));
        }

        return makeOperationResponse(ExitCode::Success,
                                     L"reconnect",
                                     target.Id,
                                     target.Name,
                                     FormatResource("Command_ReconnectSucceeded", target.Name));
    };

    auto showDevicePicker = [&]() -> Response {
        const auto result = RunControlUiAction(
            [weak = weak_from_this()]() {
                auto self = weak.lock();
                return self && self->m_trayController && self->m_trayController->ShowDevicePicker(false);
            },
            stopToken,
            deadline);
        const auto code =
            result == ControlUiActionResult::Succeeded
                ? ExitCode::Success
                : (result == ControlUiActionResult::Indeterminate ? ExitCode::Indeterminate : ExitCode::Unavailable);
        return makeOperationResponse(code,
                                     L"show",
                                     L"",
                                     L"",
                                     result == ControlUiActionResult::Succeeded ? std::wstring(_("Command_ShowOpened"))
                                                                                : std::wstring(_("Command_NotReady")));
    };

    auto showSettings = [&]() -> Response {
        const auto result = RunControlUiAction(
            [weak = weak_from_this()]() {
                auto self = weak.lock();
                return self && self->ShowSettingsWindow();
            },
            stopToken,
            deadline);
        const auto code =
            result == ControlUiActionResult::Succeeded
                ? ExitCode::Success
                : (result == ControlUiActionResult::Indeterminate ? ExitCode::Indeterminate : ExitCode::Unavailable);
        return makeOperationResponse(code,
                                     L"settings",
                                     L"",
                                     L"",
                                     result == ControlUiActionResult::Succeeded
                                         ? std::wstring(_("Command_SettingsOpened"))
                                         : std::wstring(_("Command_NotReady")));
    };

    auto showDefault = [&]() -> Response {
        SettingsData snapshot;
        {
            auto locked = m_settings->LockSharedData();
            snapshot = *locked;
        }

        std::wstring mode =
            snapshot.DefaultDevice == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
        std::optional<TargetResolution> target;
        if (snapshot.DefaultDevice == DefaultDeviceMode::SpecificDevice && !snapshot.DefaultDeviceId.empty()) {
            target = targetFromId(snapshot.DefaultDeviceId, buildDevices(false));
        } else if (!snapshot.LastConnectedIds.empty()) {
            target = targetFromId(snapshot.LastConnectedIds.front(), buildDevices(false));
        }

        if (wantsJson) {
            JsonObject root;
            root.Insert(L"ok", JsonValue::CreateBooleanValue(true));
            root.Insert(L"mode", JsonValue::CreateStringValue(winrt::hstring(mode)));
            root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redactOutput));
            root.Insert(L"id",
                        JsonValue::CreateStringValue(
                            winrt::hstring(target ? ResponseId(target->Id, redactOutput) : std::wstring())));
            root.Insert(L"displayName",
                        JsonValue::CreateStringValue(winrt::hstring(target ? target->Name : std::wstring())));
            root.Insert(L"resolved", JsonValue::CreateBooleanValue(target && target->Exists));
            root.Insert(L"connected",
                        JsonValue::CreateBooleanValue(target &&
                                                      m_deviceManager->IsDeviceConnected(winrt::hstring(target->Id))));
            return {ExitCode::Success, std::wstring(root.Stringify())};
        }

        if (!target) {
            return {ExitCode::Success, std::wstring(_("Command_DefaultMode_LastConnected")) + L"\n"};
        }
        return {ExitCode::Success, FormatResource("Command_DefaultMode_Specific", target->Name) + L"\n"};
    };

    auto setDefault = [&]() -> Response {
        auto target = resolveTarget(request);
        if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
        if (!target.Exists) {
            return makeResponse(ExitCode::NotFound, FormatResource("Command_TargetNotFound", request.Payload));
        }
        if (m_settingsController) {
            m_settingsController->SetDefaultDeviceId(target.Id);
        }
        return makeOperationResponse(ExitCode::Success,
                                     L"default-set",
                                     target.Id,
                                     target.Name,
                                     FormatResource("Command_DefaultSet", target.Name));
    };

    auto clearDefault = [&]() -> Response {
        if (m_settingsController) {
            m_settingsController->ClearDefaultDevice();
        }
        return makeOperationResponse(ExitCode::Success, L"default-clear", L"", L"", _("Command_DefaultCleared"));
    };

    auto listAliases = [&]() -> Response {
        auto devices = buildDevices(false);
        if (wantsJson) {
            JsonObject root;
            JsonArray deviceArray;
            for (auto const& device : devices) {
                auto object = deviceJson(device);
                object.Insert(L"hasAlias", JsonValue::CreateBooleanValue(!device.Alias.empty()));
                deviceArray.Append(object);
            }
            root.Insert(L"devices", deviceArray);
            root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redactOutput));
            return {ExitCode::Success, std::wstring(root.Stringify())};
        }

        if (devices.empty()) {
            return {ExitCode::Success, _("Command_AliasList_NoDevices") + L"\n"};
        }

        std::wstringstream output;
        output << _("Command_AliasList_Header") << L"\n";
        for (auto const& device : devices) {
            output << L"- " << DeviceDisplayLabel(device, redactOutput) << L": "
                   << (device.Alias.empty() ? std::wstring(_("Command_AliasNone")) : device.Alias) << L"\n";
            output << L"  ID: " << ResponseId(device.Id, redactOutput) << L"\n";
        }
        return {ExitCode::Success, output.str()};
    };

    auto splitAliasPayload = [](std::wstring const& payload) -> std::optional<std::pair<std::wstring, std::wstring>> {
        auto separator = payload.find(L'\n');
        if (separator == std::wstring::npos) return std::nullopt;
        auto target = payload.substr(0, separator);
        auto alias = payload.substr(separator + 1);
        if (target.empty() || alias.empty()) return std::nullopt;
        if (alias.find_first_of(L"\r\n") != std::wstring::npos) return std::nullopt;
        return std::pair{std::move(target), std::move(alias)};
    };

    auto setAlias = [&]() -> Response {
        auto payload = splitAliasPayload(request.Payload);
        if (!payload) return makeResponse(ExitCode::InvalidRequest, _("Command_InvalidAliasPayload"));

        auto targetRequest = request;
        targetRequest.Payload = payload->first;
        auto target = resolveTarget(targetRequest);
        if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
        if (!target.Exists) {
            return makeResponse(ExitCode::NotFound, FormatResource("Command_TargetNotFound", payload->first));
        }

        if (m_settingsController && !m_settingsController->SetDeviceAlias(target.Id, payload->second, target.RawName)) {
            return makeOperationResponse(ExitCode::OperationFailed,
                                         L"alias-set",
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_AliasSetFailed", target.Name));
        }
        return makeOperationResponse(ExitCode::Success,
                                     L"alias-set",
                                     target.Id,
                                     payload->second,
                                     FormatResource("Command_AliasSet", target.Name, payload->second));
    };

    auto clearAlias = [&]() -> Response {
        auto target = resolveTarget(request);
        if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
        if (!target.Exists) {
            return makeResponse(ExitCode::NotFound, FormatResource("Command_TargetNotFound", request.Payload));
        }

        if (m_settingsController && !m_settingsController->SetDeviceAlias(target.Id, L"", target.RawName)) {
            return makeOperationResponse(ExitCode::OperationFailed,
                                         L"alias-clear",
                                         target.Id,
                                         target.Name,
                                         FormatResource("Command_AliasClearFailed", target.Name));
        }
        auto displayName = redactOutput ? std::wstring(_("Privacy_RedactedDevice"))
                                        : (target.RawName.empty() ? target.Id : target.RawName);
        return makeOperationResponse(ExitCode::Success,
                                     L"alias-clear",
                                     target.Id,
                                     displayName,
                                     FormatResource("Command_AliasCleared", target.Name));
    };

    try {
        switch (request.Command) {
            case CommandType::Show: return showDevicePicker();
            case CommandType::Settings: return showSettings();
            case CommandType::List: return listDevices();
            case CommandType::Status: return status();
            case CommandType::DefaultShow: return showDefault();
            case CommandType::DefaultSet: return setDefault();
            case CommandType::DefaultClear: return clearDefault();
            case CommandType::AliasList: return listAliases();
            case CommandType::AliasSet: return setAlias();
            case CommandType::AliasClear: return clearAlias();
            case CommandType::DisconnectAll:
                m_deviceManager->DisconnectAll();
                return makeOperationResponse(
                    ExitCode::Success, L"disconnect-all", L"", L"", _("Command_DisconnectAllSucceeded"));
            case CommandType::ReconnectAll: {
                auto connected = m_deviceManager->GetConnectedDevices();
                for (auto const& device : connected) {
                    if (!device.Id.empty()) {
                        auto operation = m_deviceManager->ReconnectAsync(winrt::hstring(device.Id));
                        const auto waitResult = WaitForControlAsync(operation, stopToken, deadline);
                        if (waitResult != ControlWaitResult::Completed) {
                            return makeOperationResponse(ExitCode::Indeterminate,
                                                         L"reconnect-all",
                                                         device.Id,
                                                         device.Name,
                                                         waitResult == ControlWaitResult::Cancelled
                                                             ? std::wstring(_("Command_NotReady"))
                                                             : FormatResource("Command_ReconnectFailed", device.Name));
                        }
                    }
                }
                return makeOperationResponse(
                    ExitCode::Success, L"reconnect-all", L"", L"", _("Command_ReconnectAllSucceeded"));
            }
            case CommandType::Connect: {
                auto target = resolveTarget(request);
                if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
                return connectTarget(target, L"connect");
            }
            case CommandType::Disconnect: {
                auto target = resolveTarget(request);
                if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
                return disconnectTarget(target, L"disconnect");
            }
            case CommandType::Reconnect: {
                auto target = resolveTarget(request);
                if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
                return reconnectTarget(target);
            }
            case CommandType::ToggleLast: {
                auto target = resolveTarget(request);
                if (target.Code != ExitCode::Success) return makeResponse(target.Code, target.Message);
                if (m_deviceManager->IsDeviceConnected(winrt::hstring(target.Id))) {
                    return disconnectTarget(target, L"toggle");
                }
                return connectTarget(target, L"toggle");
            }
            default: return makeResponse(ExitCode::InvalidRequest, _("Command_Unsupported"));
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[App] HandleControlCommand ERROR", ex);
        return {ExitCode::Indeterminate, L""};
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[App] HandleControlCommand ERROR", ex);
        return {ExitCode::Indeterminate, L""};
    } catch (...) {
        util::DebugTraceUnknownException(L"[App] HandleControlCommand ERROR");
        return {ExitCode::Indeterminate, L""};
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Handlers /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void ApplicationHost::OnDeviceConnected(winrt::hstring const& id) {
    if (m_exiting.load() || !m_deviceManager) return;
    DebugTrace(L"[App] OnDeviceConnected: {0}", std::wstring(id));

    if (!m_deviceManager->IsDeviceConnected(id)) {
        return;
    }
    m_powerTransitionCoordinator.NotifyDeviceConnected(std::wstring_view(id));
    if (!m_settings) return;

    winrt::hstring rawDeviceName = id;
    if (auto displayName = m_deviceManager->GetConnectionDisplayName(id)) {
        rawDeviceName = winrt::hstring(*displayName);
    }

    auto const idString = std::wstring(id);
    const bool validDeviceId =
        !idString.empty() && apc::limits::IsBoundedUtf16(idString, apc::limits::c_maxDeviceIdCharacters);
    auto deviceName =
        apc::limits::TruncateUtf16(std::wstring_view(rawDeviceName), apc::limits::c_maxDeviceNameCharacters);
    if (deviceName.empty()) {
        deviceName = apc::limits::TruncateUtf16(idString, apc::limits::c_maxDeviceNameCharacters);
    }
    bool addedNew = false;
    bool settingsChanged = false;
    bool devicePresentationChanged = false;
    try {
        auto locked = m_settings->LockExclusiveData();
        auto const existingDevice = std::ranges::find(locked->Devices, idString, &DeviceSettings::Id);
        auto const hasExistingDevice = existingDevice != locked->Devices.end();
        auto const existingDeviceIndex =
            hasExistingDevice ? std::optional<std::size_t>(std::distance(locked->Devices.begin(), existingDevice))
                              : std::nullopt;
        auto const addDevice =
            !hasExistingDevice && validDeviceId && locked->Devices.size() < apc::limits::c_maxPersistedDeviceCount;
        auto const updateDeviceName = hasExistingDevice && existingDevice->Name != deviceName;
        auto const promoteMru =
            validDeviceId && (locked->LastConnectedIds.empty() || locked->LastConnectedIds.front() != idString ||
                              std::ranges::count(locked->LastConnectedIds, idString) != 1);

        if (addDevice || updateDeviceName || promoteMru) {
            auto& data = locked.Mutate();
            settingsChanged = true;
            if (addDevice) {
                DeviceSettings newDevice;
                newDevice.Id = idString;
                newDevice.Name = deviceName;
                newDevice.ConnectOnStartup = data.GlobalConnectOnStartup;
                newDevice.ReconnectOnConnectionLoss = data.GlobalReconnectOnConnectionLoss;
                data.Devices.push_back(std::move(newDevice));
                addedNew = true;
                devicePresentationChanged = true;
            } else if (updateDeviceName) {
                data.Devices[*existingDeviceIndex].Name = deviceName;
                devicePresentationChanged = true;
            }
            if (promoteMru) {
                static_cast<void>(AutoReconnectPlanner::PromoteMostRecentlyConnected(data.LastConnectedIds, idString));
            }
        }
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

    if (settingsChanged) {
        m_settingsSaver.RequestSave();
        if (devicePresentationChanged) m_settingsWindowPresenter.RefreshKnownDevicesIfOpen();
        if (addedNew) {
            DebugTrace(L"[App] New device added to settings: {0}", std::wstring(rawDeviceName));
        }
    }

    try {
        auto locked = m_settings->LockSharedData();
        bool reconnectOnConnectionLoss = locked->GlobalReconnectOnConnectionLoss;
        auto it = std::ranges::find_if(locked->Devices, [&](const auto& d) { return d.Id == id; });
        if (it != locked->Devices.end()) {
            reconnectOnConnectionLoss = reconnectOnConnectionLoss || it->ReconnectOnConnectionLoss;
        }
        m_deviceManager->SetReconnectOnConnectionLoss(id, reconnectOnConnectionLoss);
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
    if (m_exiting.load() || !m_settings) return;
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
    if (m_exiting.load() || !m_settings) return;
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
    if (m_exiting.load() || !m_settings) return;
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
            case PBT_APMRESUMESUSPEND: host->HandlePowerResume(); return TRUE;
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

    if (msg == WM_TIMER && host->m_settingsSaver.HandleWindowTimer(wParam)) {
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
