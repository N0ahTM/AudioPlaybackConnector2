#pragma once

#include <ui/App/App.xaml.g.h>

#include <services/NotificationService.hpp>
#include <services/TrayController.hpp>

class Settings;
class DeviceManager;

#include <atomic>

namespace winrt::AudioPlaybackConnector2::implementation {
struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e);

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Accessors ////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    static App* GetInstance() { return s_instance.load(); }
    ::Settings& GetSettings() { return *m_settings; }
    std::shared_ptr<::DeviceManager> GetDeviceManager() { return m_deviceManager; }
    HWND GetWindowHandle() const { return m_hwnd; }
    winrt::Microsoft::UI::Xaml::Window GetMainWindow() const { return m_mainWindow; }

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ////////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void ShowSettingsWindow();
    void ExitApplication();

    void RunOnUIThread(std::function<void()> work);

private:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Setup //////////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void SetupMainWindow();
    void OnMainWindowLoaded(winrt::Microsoft::UI::Xaml::Controls::Grid const& root);
    void InitializeTray();
    void InitializeNotifications();
    void InitializeDeviceManager();
    void SetupDeviceEvents();
    void TeardownDeviceEvents();
    void TryAutoReconnect();
    void SaveLastConnectedDevices();
    void HandlePowerSuspend();
    void HandlePowerResume();
    [[nodiscard]] winrt::hstring ResolveKnownDeviceName(winrt::hstring const& id) const;

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Event Handlers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void OnDeviceConnected(winrt::hstring const& id);
    void OnDeviceDisconnected(winrt::hstring const& id);
    void OnConnectionError(winrt::hstring const& id, winrt::hstring msg);
    void OnAutoReconnectTriggered(winrt::hstring const& id);
    void OnAutoReconnectFailed(winrt::hstring const& id);

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Window Subclass ////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables ////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    static std::atomic<App*> s_instance;
    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    winrt::Microsoft::UI::Xaml::Window m_settingsWindow{nullptr};
    HWND m_hwnd = nullptr;

    std::unique_ptr<::Settings> m_settings;
    std::shared_ptr<::DeviceManager> m_deviceManager;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};

    std::size_t m_deviceConnectedToken = 0;
    std::size_t m_deviceDisconnectedToken = 0;
    std::size_t m_connectionErrorToken = 0;
    std::size_t m_autoReconnectTriggeredToken = 0;
    std::size_t m_autoReconnectFailedToken = 0;
    std::size_t m_deviceStatusChangedToken = 0;

    std::shared_ptr<NotificationService> m_notificationService;
    std::shared_ptr<TrayController> m_trayController;
    wil::unique_handle m_singleInstanceMutex;
    static inline UINT s_wmTaskbarCreated = 0;
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    ULONG_PTR m_gdiplusToken = 0;
    bool m_notificationsAvailable = false;
    std::atomic<bool> m_exiting = false;
    bool m_powerSuspended = false;
    winrt::Windows::System::Threading::ThreadPoolTimer m_resumeReconnectTimer{nullptr};
};
} // namespace winrt::AudioPlaybackConnector2::implementation
