#pragma once

#include <app/DeviceEventRouter.hpp>
#include <app/SingleInstanceGuard.hpp>
#include <app/PowerTransitionCoordinator.hpp>
#include <app/SettingsWindowPresenter.hpp>
#include <ui/App/App.xaml.g.h>

#include <services/NotificationService.hpp>
#include <services/SettingsController.hpp>
#include <services/TrayController.hpp>

class Settings;
class DeviceManager;

#include <atomic>

namespace winrt::AudioPlaybackConnector2::implementation {
struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowSettingsWindow();
    void ExitApplication();

    void RunOnUIThread(std::function<void()> work);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Setup /////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetupMainWindow();
    void OnMainWindowLoaded(winrt::Microsoft::UI::Xaml::Controls::Grid const& root);
    void InitializeTray();
    void InitializeNotifications();
    void InitializeDeviceManager();
    void SetupDeviceEvents();
    void TeardownDeviceEvents();
    void TryAutoReconnect();
    winrt::fire_and_forget CheckForUpdatesOnStartupAsync();
    void SaveLastConnectedDevices();
    void HandlePowerSuspend();
    void HandlePowerResume();
    void ToggleLastConnectedDeviceFromTray();
    void RefreshTrayVisualState(bool forceErrorWhenIdle = false);
    [[nodiscard]] winrt::hstring ResolveKnownDeviceName(winrt::hstring const& id) const;

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
    SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    HWND m_hwnd = nullptr;

    std::unique_ptr<::Settings> m_settings;
    std::shared_ptr<::DeviceManager> m_deviceManager;
    std::shared_ptr<ISettingsController> m_settingsController;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};

    std::shared_ptr<NotificationService> m_notificationService;
    std::shared_ptr<TrayController> m_trayController;
    DeviceEventRouter m_deviceEventRouter;
    SingleInstanceGuard m_singleInstanceGuard;
    static inline UINT s_wmTaskbarCreated = 0;
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    ULONG_PTR m_gdiplusToken = 0;
    bool m_notificationsAvailable = false;
    std::atomic<bool> m_exiting = false;
    PowerTransitionCoordinator m_powerTransitionCoordinator{m_exiting};
    SettingsWindowPresenter m_settingsWindowPresenter;
};
} // namespace winrt::AudioPlaybackConnector2::implementation
