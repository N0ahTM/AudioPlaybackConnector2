// cppcheck-suppress-file syntaxError
#pragma once

#include <ui/App/App.xaml.g.h>

class TrayIcon;
class TrayContextMenu;
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
    void UpdateTrayTooltip();

    void RunOnUIThread(std::function<void()> work);

private:
    void EnsureDevicePickerViewCreated();
    void PreloadDevicePicker();
    winrt::Microsoft::UI::Xaml::Controls::Flyout CreatePickerFlyout();
    static void StripFlyoutPresenterStyle(winrt::Microsoft::UI::Xaml::DependencyObject const& content);
    std::optional<POINT> CalculateSettingsWindowPosition() const;
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Setup //////////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void SetupMainWindow();
    void OnMainWindowLoaded(winrt::Microsoft::UI::Xaml::Controls::Grid const& root);
    void InitializeTray();
    void InitializeDeviceManager();
    void InitializeContextMenu();
    void LaunchBluetoothSettings();
    void SetupDeviceEvents();
    void TeardownDeviceEvents();
    void TryAutoReconnect();

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Tray Handlers //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void OnTrayIconLeftClick();
    void OnTrayIconRightClick();
    void OnTrayIconMessage(WPARAM wParam, LPARAM lParam);

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Event Handlers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void OnDeviceConnected(winrt::hstring const& id);
    void OnDeviceDisconnected(winrt::hstring const& id);
    void OnConnectionError(winrt::hstring const& id, winrt::hstring msg);
    void OnAutoReconnectTriggered(winrt::hstring const& id);

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

    std::unique_ptr<::TrayIcon> m_trayIcon;
    std::unique_ptr<::TrayContextMenu> m_contextMenu;
    std::unique_ptr<::Settings> m_settings;
    std::shared_ptr<::DeviceManager> m_deviceManager;
    winrt::Microsoft::UI::Xaml::Controls::Flyout m_pickerFlyout{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};

    std::size_t m_deviceConnectedToken = 0;
    std::size_t m_deviceDisconnectedToken = 0;
    std::size_t m_connectionErrorToken = 0;
    std::size_t m_autoReconnectTriggeredToken = 0;
    std::size_t m_deviceStatusChangedToken = 0;
    std::size_t m_themeChangedToken = 0;
    winrt::AudioPlaybackConnector2::DevicePickerView m_devicePickerView{nullptr};

    UINT m_trayCallbackMsg = WM_APP + 1;
    static inline UINT s_wmTaskbarCreated = 0;
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    ULONG_PTR m_gdiplusToken = 0;
    bool m_devicePickerPreloaded = false;
    std::atomic<bool> m_exiting = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation
