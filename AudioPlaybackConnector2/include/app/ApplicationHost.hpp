#pragma once

#include <app/AdaptiveResourcePolicy.hpp>
#include <app/ControlUiActionGate.hpp>
#include <app/DeviceEventRouter.hpp>
#include <app/PowerTransitionCoordinator.hpp>
#include <app/ResourcePressureMonitor.hpp>
#include <app/SettingsWindowPresenter.hpp>
#include <app/SingleInstanceGuard.hpp>

#include <services/CommandLineControlServer.hpp>
#include <services/NotificationService.hpp>
#include <services/SettingsController.hpp>
#include <services/TrayController.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>

class DeviceManager;
class Settings;
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
    void OnMainWindowLoaded(winrt::Microsoft::UI::Xaml::Controls::Grid const& root);
    void InitializeTray();
    void InitializeNotifications();
    void InitializeDeviceManager();
    void InitializeCommandLineControl();
    void InitializeAdaptiveResources() noexcept;
    void SetupDeviceEvents();
    void TeardownDeviceEvents();
    void TryAutoReconnect();
    winrt::fire_and_forget CheckForUpdatesOnStartupAsync();
    void SaveLastConnectedDevices(bool saveImmediately = false);
    void ScheduleDeferredSettingsSave();
    void HandlePowerSuspend();
    void HandlePowerResume();
    void ToggleLastConnectedDeviceFromTray();
    void RefreshTrayVisualState(bool forceErrorWhenIdle = false, std::wstring_view reason = L"unspecified");
    void HandleResourcePressureSnapshot(ResourcePressureSnapshot snapshot);
    void EvaluateAdaptiveResources(bool userInteraction, std::wstring_view reason) noexcept;
    void ScheduleAdaptiveResourceEvaluation(std::optional<AdaptiveResourcePolicy::TimePoint> reevaluateAt) noexcept;
    [[nodiscard]] winrt::hstring ResolveKnownDeviceName(winrt::hstring const& id) const;
    [[nodiscard]] std::optional<std::wstring> ResolveDefaultDeviceId() const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool ShowSettingsWindow();
    void ExitApplication();
    apc::control::Response
    HandleControlCommand(apc::control::Request const& request, std::stop_token stopToken, std::uint64_t deadline);
    void PerformTeardown(bool saveLastConnected) noexcept;
    void RunOnUIThread(std::function<void()> work);
    ControlUiActionResult
    RunControlUiAction(std::function<bool()> work, std::stop_token stopToken, std::uint64_t deadline);

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

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    winrt::event_token m_mainWindowLoadedToken{};
    HWND m_hwnd = nullptr;

    std::shared_ptr<::Settings> m_settings;
    std::shared_ptr<::DeviceManager> m_deviceManager;
    std::shared_ptr<ISettingsController> m_settingsController;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};

    std::shared_ptr<NotificationService> m_notificationService;
    std::shared_ptr<UpdateCoordinator> m_updateCoordinator;
    std::shared_ptr<TrayController> m_trayController;
    CommandLineControlServer m_commandLineControlServer;
    std::mutex m_controlMutationMutex;
    DeviceEventRouter m_deviceEventRouter;
    SingleInstanceGuard m_singleInstanceGuard;
    static inline UINT s_wmTaskbarCreated = 0;
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    static constexpr UINT_PTR c_timerTransientTrayError = 0x41504333;
    static constexpr UINT_PTR c_timerAdaptiveResources = 0x41504334;
    static constexpr UINT c_transientTrayErrorMs = 3000;
    std::chrono::steady_clock::time_point m_trayErrorUntil{};
    AdaptiveResourcePolicy m_adaptiveResourcePolicy;
    ResourcePressureValues m_resourcePressureValues;
    std::unique_ptr<ResourcePressureMonitor> m_resourcePressureMonitor;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_adaptiveResourceFallbackTimer{nullptr};
    AdaptiveActionRetryBackoff m_adaptiveActionRetryBackoff;
    AdaptiveScheduleState m_adaptiveScheduleState;
    std::mutex m_resourceAuthorizationMutex;
    std::optional<AdaptiveResourcePolicy::TimePoint> m_lastResourcePressureObservedAt;
    std::uint64_t m_lastResourcePressureSequence = 0;
    std::uint64_t m_latestConstrainedResourcePressureSequence = 0;
    ULONG_PTR m_gdiplusToken = 0;
    std::atomic<bool> m_exiting = false;
    std::atomic<bool> m_settingsSavePending = false;
    PowerTransitionCoordinator m_powerTransitionCoordinator{m_exiting};
    SettingsWindowPresenter m_settingsWindowPresenter;
};
