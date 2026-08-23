#pragma once

#include <app/AdaptiveResourcePolicy.hpp>
#include <app/AdaptiveResourceDiagnostics.hpp>
#include <app/AppController.hpp>
#include <app/ControlUiActionGate.hpp>
#include <app/DeviceEventRouter.hpp>
#include <app/LegacyAppUseCaseBridge.hpp>
#include <app/PowerTransitionCoordinator.hpp>
#include <app/ResourcePressureMonitor.hpp>
#include <app/SettingsWindowPresenter.hpp>
#include <app/SingleInstanceGuard.hpp>
#include <app/StartupTaskCoordinator.hpp>
#include <app/UiRefreshCoalescer.hpp>

#include <core/DeviceService.hpp>
#include <core/SettingsStore.hpp>

#include <services/CommandLineControlServer.hpp>
#include <services/NotificationService.hpp>
#include <services/SettingsController.hpp>
#include <services/TrayController.hpp>

#include <control/ControlCommandAdapter.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>

class UpdateCoordinator;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Host //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ApplicationHost : public std::enable_shared_from_this<ApplicationHost> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    ApplicationHost();
    ~ApplicationHost();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Start();
    void Shutdown() noexcept;

private:
    using ControlUiActionResult = ControlUiActionGate::Result;
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Setup /////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetupMainWindow();
    void StartMainWindowLoadedWatchdog();
    void StopMainWindowLoadedWatchdog() noexcept;
    void OnMainWindowLoaded(winrt::Microsoft::UI::Xaml::Controls::Grid const& root) noexcept;
    void FailStartup(std::wstring_view stage) noexcept;
    void InitializeTray();
    void InitializeNotifications();
    void InitializeDeviceService();
    void InitializeAppController();
    void InitializeCommandLineControl();
    void InitializeAdaptiveResources() noexcept;
    void SetupDeviceEvents();
    void TeardownDeviceEvents();
    void TryAutoReconnect();
    winrt::fire_and_forget CheckForUpdatesOnStartupAsync();
    void HandlePowerSuspend();
    void HandlePowerResume();
    void ExecuteTrayCommand(
        apc::app::AppCommand command,
        apc::app::AppCommandContext::CompletionMode completion = apc::app::AppCommandContext::CompletionMode::Detached);
    [[nodiscard]] bool RefreshTrayVisualState(bool forceErrorWhenIdle = false,
                                              std::wstring_view reason = L"unspecified");
    void ScheduleDeviceVisualRefresh(bool forceErrorWhenIdle = false,
                                     bool inventoryChanged = false,
                                     bool refreshTray = true);
    void QueueDeviceVisualRefreshDrain() noexcept;
    void DrainDeviceVisualRefresh() noexcept;
    [[nodiscard]] bool ScheduleNativeDeviceVisualRefreshRetry(std::chrono::milliseconds delay) noexcept;
    void CancelNativeDeviceVisualRefreshRetry() noexcept;
    void HandleResourcePressureSnapshot(ResourcePressureSnapshot snapshot);
    void EvaluateAdaptiveResources(bool userInteraction, std::wstring_view reason) noexcept;
    void ScheduleAdaptiveResourceEvaluation(std::optional<AdaptiveResourcePolicy::TimePoint> reevaluateAt) noexcept;
    [[nodiscard]] winrt::hstring ResolveKnownDeviceName(winrt::hstring const& id) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool ShowSettingsWindow();
    void ExitApplication() noexcept;
    [[nodiscard]] bool CloseMainWindow(std::wstring_view reason) noexcept;
    [[nodiscard]] bool PerformTeardown(SettingsShutdownMode settingsShutdownMode) noexcept;
    [[nodiscard]] bool RunOnUIThread(std::function<void()> work) noexcept;
    [[nodiscard]] bool QueueUiFallbackWork(std::function<void()> work) noexcept;
    void DrainUiFallbackWork() noexcept;
    ControlUiActionResult RunControlUiAction(std::function<bool()> work, apc::app::AppCommandContext const& context);

    void PublishDeviceFact(apc::app::LegacyAppUseCaseBridge::DeviceFact fact) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Event Handlers /////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void OnDeviceConnected(winrt::hstring const& id);
    void OnDeviceDisconnected(winrt::hstring const& id);
    void OnConnectionError(winrt::hstring const& id, winrt::hstring msg);
    void OnAutoReconnectTriggered(winrt::hstring const& id);
    void OnAutoReconnectFailed(winrt::hstring const& id);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Window Subclass ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static LRESULT CALLBACK
    SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept;
    static void CALLBACK DeviceVisualRefreshRetryTimerCallback(PTP_CALLBACK_INSTANCE,
                                                               void* context,
                                                               PTP_TIMER) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    winrt::event_token m_mainWindowLoadedToken{};
    HWND m_hwnd = nullptr;

    std::shared_ptr<SettingsStore> m_settingsStore;
    std::shared_ptr<apc::device::DeviceService> m_deviceService;
    std::shared_ptr<ISettingsController> m_settingsController;
    std::shared_ptr<StartupTaskCoordinator> m_startupTaskCoordinator;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_mainWindowLoadedWatchdog{nullptr};

    std::shared_ptr<NotificationService> m_notificationService;
    std::shared_ptr<UpdateCoordinator> m_updateCoordinator;
    std::shared_ptr<TrayController> m_trayController;
    std::shared_ptr<apc::app::LegacyAppUseCaseBridge> m_appBridge;
    std::unique_ptr<apc::app::AppController> m_appController;
    std::unique_ptr<apc::control::ControlCommandAdapter> m_controlCommandAdapter;
    CommandLineControlServer m_commandLineControlServer;
    std::mutex m_uiFallbackWorkMutex;
    std::deque<std::function<void()>> m_uiFallbackWork;
    bool m_uiFallbackMessagePending = false;
    DeviceEventRouter m_deviceEventRouter;
    SingleInstanceGuard m_singleInstanceGuard;
    static inline UINT s_wmTaskbarCreated = 0;
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    static constexpr UINT_PTR c_timerTransientTrayError = 0x41504333;
    static constexpr UINT_PTR c_timerAdaptiveResources = 0x41504334;
    static constexpr UINT_PTR c_timerDeviceVisualRefreshRetry = 0x41504335;
    static constexpr UINT c_messageDrainUiFallbackWork = WM_APP + 2;
    static constexpr UINT c_messageDrainDeviceVisualRefresh = WM_APP + 3;
    static constexpr UINT c_transientTrayErrorMs = 3000;
    static constexpr UiRefreshCoalescer::Flags c_visualRefreshRequested = 1U << 0;
    static constexpr UiRefreshCoalescer::Flags c_visualRefreshForceError = 1U << 1;
    static constexpr UiRefreshCoalescer::Flags c_visualRefreshInventoryChanged = 1U << 2;
    std::chrono::steady_clock::time_point m_trayErrorUntil{};
    std::wstring m_transientTrayErrorTooltip;
    bool m_connectingAnimationTimerActive = false;
    AdaptiveResourcePolicy m_adaptiveResourcePolicy;
    ResourcePressureValues m_resourcePressureValues;
    std::unique_ptr<ResourcePressureMonitor> m_resourcePressureMonitor;
    HPOWERNOTIFY m_powerSavingStatusNotification = nullptr;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_adaptiveResourceFallbackTimer{nullptr};
    AdaptiveActionRetryBackoff m_adaptiveActionRetryBackoff;
    AdaptiveScheduleState m_adaptiveScheduleState;
    std::mutex m_resourceAuthorizationMutex;
    AdaptiveResourceDiagnostics m_adaptiveResourceDiagnostics;
    std::optional<AdaptiveResourcePolicy::TimePoint> m_lastResourcePressureObservedAt;
    std::uint64_t m_lastResourcePressureSequence = 0;
    std::uint64_t m_latestConstrainedResourcePressureSequence = 0;
    ULONG_PTR m_gdiplusToken = 0;
    std::atomic<bool> m_exiting = false;
    std::atomic<bool> m_started = false;
    std::atomic<bool> m_teardownWindowCloseSucceeded = true;
    bool m_windowSubclassInstalled = false;
    UiRefreshCoalescer m_deviceVisualRefreshCoalescer;
    unsigned int m_deviceVisualRefreshConsecutiveFailures = 0;
    std::mutex m_deviceVisualRefreshRetryTimerMutex;
    wil::unique_threadpool_timer m_deviceVisualRefreshRetryTimer;
    PowerTransitionCoordinator m_powerTransitionCoordinator{m_exiting};
    SettingsWindowPresenter m_settingsWindowPresenter;
};
