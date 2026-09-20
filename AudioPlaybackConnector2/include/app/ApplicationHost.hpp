#pragma once

#include <core/StringResources.hpp>

#include <app/AppController.hpp>
#include <app/AdaptiveResourceController.hpp>
#include <app/PowerTransitionCoordinator.hpp>
#include <app/SettingsWindowPresenter.hpp>
#include <app/SingleInstanceGuard.hpp>
#include <app/StartupTaskCoordinator.hpp>
#include <app/UiRefreshScheduler.hpp>
#include <app/UiDispatcher.hpp>

#include <core/DeviceService.hpp>
#include <core/SettingsStore.hpp>

#include <services/CommandLineControlServer.hpp>
#include <services/NotificationService.hpp>
#include <services/TrayController.hpp>

#include <control/ControlCommandAdapter.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Host //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ApplicationHost : public std::enable_shared_from_this<ApplicationHost>, public apc::app::AppPresentation {
public:
    apc::app::AppUiActionResult PresentDevicePicker(apc::app::DevicePickerOpenMode mode,
                                                    apc::app::AppCommandContext const& context) override;
    apc::app::AppUiActionResult PresentSettings(apc::app::AppCommandContext const& context) override;
    apc::app::AppSnapshot::ResourceStatusSnapshot ResourceStatus() const override;
    std::uint64_t PickerOpenedGeneration() const override;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit ApplicationHost(util::LogSink log, util::EmergencyLog emergency);
    ~ApplicationHost();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Start();
    void Shutdown() noexcept;

private:
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
    void SetupDeviceEvents();
    void HandlePowerSuspend();
    void HandlePowerResume();
    void ScheduleDeviceVisualRefresh(UiRefreshScheduler::Flags refresh);
    bool RefreshDeviceVisuals(UiRefreshScheduler::Flags flags);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool ShowSettingsWindow();
    void ExitApplication() noexcept;
    [[nodiscard]] bool CloseMainWindow(std::wstring_view reason) noexcept;
    [[nodiscard]] bool PerformTeardown(SettingsShutdownMode settingsShutdownMode) noexcept;

    void HandleAppEvent(apc::app::AppController::EventNotification const& event);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Window Subclass ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static LRESULT CALLBACK
    SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    util::LogSink m_log;
    std::shared_ptr<StringResources> m_strings = std::make_shared<StringResources>();
    util::EmergencyLog m_emergencyLog;
    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    winrt::event_token m_mainWindowLoadedToken{};
    HWND m_hwnd = nullptr;

    std::shared_ptr<SettingsStore> m_settingsStore;
    std::shared_ptr<apc::device::DeviceService> m_deviceService;
    std::shared_ptr<StartupTaskCoordinator> m_startupTaskCoordinator;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_mainWindowLoadedWatchdog{nullptr};

    std::shared_ptr<NotificationService> m_notificationService;
    std::shared_ptr<TrayController> m_trayController;
    std::shared_ptr<apc::app::AppController> m_appController;
    std::unique_ptr<apc::control::ControlCommandAdapter> m_controlCommandAdapter;
    CommandLineControlServer m_commandLineControlServer;
    apc::app::AppController::Subscription m_appEventSubscription;
    std::uint64_t m_lastAppEventRevision = 0;
    std::wstring m_appliedLanguage;
    std::optional<bool> m_appliedBackdrop;
    SingleInstanceGuard m_singleInstanceGuard;
    UINT m_taskbarCreatedMessage = 0;
    static constexpr UiRefreshScheduler::Flags c_visualRefreshRequested = 1U << 0;
    static constexpr UiRefreshScheduler::Flags c_visualRefreshForceError = 1U << 1;
    static constexpr UiRefreshScheduler::Flags c_visualRefreshInventoryChanged = 1U << 2;
    // Assigned once before publishing the controller; retained through its final snapshots.
    const std::shared_ptr<AdaptiveResourceController> m_adaptiveResources =
        std::make_shared<AdaptiveResourceController>(m_log);
    ULONG_PTR m_gdiplusToken = 0;
    std::atomic<bool> m_exiting = false;
    std::atomic<bool> m_started = false;
    std::atomic<bool> m_teardownWindowCloseSucceeded = true;
    bool m_windowSubclassInstalled = false;
    std::shared_ptr<UiDispatcher> m_uiDispatcher;
    std::unique_ptr<UiRefreshScheduler> m_visualRefresh;
    PowerTransitionCoordinator m_powerTransitionCoordinator{m_exiting, {}, m_log};
    SettingsWindowPresenter m_settingsWindowPresenter{m_log, m_strings};
};
