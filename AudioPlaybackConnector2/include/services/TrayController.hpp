#pragma once

#include <ui/TrayIcon.hpp>
#include <ui/TrayContextMenu.hpp>
#include <DevicePickerView/DevicePickerView.xaml.h>
#include <ui/WindowPlacement.hpp>

#include <functional>
#include <memory>
#include <atomic>
#include <string_view>

class DeviceManager;
class Settings;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Controller ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class TrayController : public std::enable_shared_from_this<TrayController> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callback Types ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ShowSettingsCallback = std::move_only_function<void()>;
    using ExitCallback = std::move_only_function<void()>;
    using DeviceActionCallback = std::move_only_function<void(winrt::hstring)>;
    using BulkDeviceActionCallback = std::move_only_function<void()>;
    using ToggleDeviceCallback = std::move_only_function<void()>;
    using ResourceStateChangedCallback = std::move_only_function<void(bool userInteraction)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    TrayController();
    ~TrayController();

    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;
    TrayController(TrayController&&) = delete;
    TrayController& operator=(TrayController&&) = delete;

    void Initialize(HWND hwnd, winrt::Microsoft::UI::Xaml::Window mainWindow);
    void SetDeviceManager(std::shared_ptr<DeviceManager> deviceManager);
    void SetSettings(std::shared_ptr<Settings> settings);
    void PreloadDevicePicker() noexcept;
    void ReleaseDevicePicker() noexcept;
    void Teardown() noexcept;
    [[nodiscard]] bool IsDevicePickerLoaded() const noexcept;
    [[nodiscard]] bool IsDevicePickerPreloadInitialized() const noexcept;
    [[nodiscard]] bool IsDevicePickerVisibleOrTransitioning() const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetCallbacks(ShowSettingsCallback showSettings,
                      ExitCallback exit,
                      DeviceActionCallback connect,
                      DeviceActionCallback disconnect,
                      DeviceActionCallback reconnect,
                      ToggleDeviceCallback toggleDevice,
                      BulkDeviceActionCallback disconnectAll = nullptr,
                      BulkDeviceActionCallback reconnectAll = nullptr);
    void SetResourceStateChangedCallback(ResourceStateChangedCallback callback);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowTrayMenu();
    [[nodiscard]] bool ShowDevicePicker(bool toggleIfOpen = true) noexcept;
    void UpdateTooltip(std::wstring_view text);
    void UpdateTooltipFromConnections();
    void RefreshDevicePickerState(bool reconcileTrayState = true);
    void OnThemeChanged();
    void AdvanceConnectingFrame();
    void Reregister();
    void SetState(TrayIconState state);
    [[nodiscard]] util::SettingsWindowPlacement GetSettingsWindowPlacement() const;

    void HandleTrayMessage(WPARAM wParam, LPARAM lParam) noexcept;
    [[nodiscard]] UINT TrayCallbackMessage() const noexcept { return m_trayCallbackMsg; }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool EnsureDevicePickerViewCreated() noexcept;
    void TryHideDevicePicker() noexcept;
    void ReleaseDevicePickerOnUIThread() noexcept;
    void LaunchBluetoothSettings();
    winrt::Microsoft::UI::Xaml::Controls::Flyout CreatePickerFlyout();
    void ReconcileTrayStateFromDeviceSnapshot(std::wstring_view reason);
    [[nodiscard]] util::SettingsWindowPlacement CalculateSettingsWindowPlacement() const;
    [[nodiscard]] bool IsCursorOverTrayIcon() const;
    void OnTrayIconDoubleClick();
    void NotifyResourceStateChanged(bool userInteraction) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    HWND m_hwnd = nullptr;
    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};
    std::shared_ptr<DeviceManager> m_deviceManager;
    std::shared_ptr<Settings> m_settings;

    std::unique_ptr<TrayIcon> m_trayIcon;
    std::unique_ptr<TrayContextMenu> m_contextMenu;
    winrt::Microsoft::UI::Xaml::Controls::Flyout m_pickerFlyout{nullptr};
    winrt::AudioPlaybackConnector2::DevicePickerView m_devicePickerView{nullptr};

    ShowSettingsCallback m_showSettingsCallback;
    ExitCallback m_exitCallback;
    DeviceActionCallback m_connectCallback;
    DeviceActionCallback m_disconnectCallback;
    DeviceActionCallback m_reconnectCallback;
    BulkDeviceActionCallback m_disconnectAllCallback;
    BulkDeviceActionCallback m_reconnectAllCallback;
    ToggleDeviceCallback m_toggleDeviceCallback;
    ResourceStateChangedCallback m_resourceStateChangedCallback;

    UINT m_trayCallbackMsg = WM_APP + 1;
    std::size_t m_themeChangedToken = 0;
    ULONGLONG m_lastLeftClickTick = 0;
    ULONGLONG m_lastRightClickTick = 0;
    ULONGLONG m_lastLeftDoubleClickTick = 0;
    ULONGLONG m_lastPickerClosedOverTrayIconTick = 0;
    bool m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;

    enum class PickerFlyoutState {
        Closed,
        Opening,
        Open,
        Closing,
    };
    std::atomic<PickerFlyoutState> m_pickerFlyoutState{PickerFlyoutState::Closed};

    bool m_devicePickerPreloadInitialized = false;
    bool m_releaseDevicePickerPending = false;
    std::atomic_bool m_isTearingDown = false;
};
