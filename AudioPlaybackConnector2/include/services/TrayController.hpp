#pragma once

#include <core/StringResources.hpp>
#include <memory>

#include <app/AppController.hpp>
#include <ui/TrayIcon.hpp>
#include <ui/TrayContextMenu.hpp>
#include <DevicePickerView/DevicePickerView.xaml.h>
#include <ui/WindowPlacement.hpp>
#include <core/ThemeHelper.hpp>

#include <functional>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <stop_token>
#include <atomic>
#include <cstdint>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Controller ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class TrayController : public std::enable_shared_from_this<TrayController> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callback Types ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ShowHelpCallback = std::move_only_function<void()>;
    using ExitCallback = std::move_only_function<void()>;
    using ResourceStateChangedCallback = std::move_only_function<void(bool userInteraction)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit TrayController(util::LogSink log, std::shared_ptr<StringResources const> strings);
    ~TrayController();

    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;
    TrayController(TrayController&&) = delete;
    TrayController& operator=(TrayController&&) = delete;

    void Initialize(HWND hwnd,
                    winrt::Microsoft::UI::Xaml::Window mainWindow,
                    std::weak_ptr<apc::app::AppController> controller,
                    ExitCallback exit,
                    ShowHelpCallback showHelp);
    void PreloadDevicePicker() noexcept;
    void ReleaseDevicePicker() noexcept;
    void Teardown() noexcept;
    [[nodiscard]] bool IsDevicePickerLoaded() const noexcept;
    [[nodiscard]] bool IsDevicePickerPreloadInitialized() const noexcept;
    [[nodiscard]] bool IsDevicePickerVisibleOrTransitioning() const noexcept;
    [[nodiscard]] uint64_t DevicePickerOpenedGeneration() const noexcept;
    // Background control callers only; the Opened event needs the UI dispatcher.
    [[nodiscard]] bool WaitForDevicePickerOpened(std::uint64_t previousGeneration,
                                                 std::stop_token stop,
                                                 std::chrono::steady_clock::time_point deadline);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetResourceStateChangedCallback(ResourceStateChangedCallback callback);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void ShowTrayMenu();
    [[nodiscard]] bool ShowDevicePicker(bool toggleIfOpen = true) noexcept;
    [[nodiscard]] bool RefreshVisualState(bool forceErrorWhenIdle = false);
    void SetConnectionError(apc::app::DeviceConnectionErrorEvent::Reason reason);
    // UI-thread only; true asks the host's coalescer to refresh after expiry or a failed frame.
    [[nodiscard]] bool OnVisualTimer(UINT_PTR timerId) noexcept;
    [[nodiscard]] bool RefreshDevicePickerState() noexcept;
    [[nodiscard]] bool InvalidateDevicePickerInventory() noexcept;
    void OnThemeChanged();
    void ApplyLanguage();
    void OnSettingChange(LPARAM setting);
    void SetSystemBackdropEffectsEnabled(bool enabled) noexcept;
    void Reregister();
    [[nodiscard]] util::SettingsWindowPlacement GetSettingsWindowPlacement() const;

    void HandleTrayMessage(WPARAM wParam, LPARAM lParam) noexcept;
    [[nodiscard]] UINT TrayCallbackMessage() const noexcept { return m_trayCallbackMsg; }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] bool AdvanceConnectingFrame() noexcept;
    [[nodiscard]] bool ApplyPendingTrayUpdates() noexcept;
    [[nodiscard]] bool EnsureDevicePickerViewCreated() noexcept;
    void TryHideDevicePicker() noexcept;
    void ShowSettingsAfterPickerClosed();
    void ReleaseDevicePickerOnUIThread() noexcept;
    void LaunchBluetoothSettings();
    winrt::Microsoft::UI::Xaml::Controls::Flyout CreatePickerFlyout();
    [[nodiscard]] bool IsCursorOverTrayIcon() const;
    void OnTrayIconDoubleClick();
    void NotifyResourceStateChanged(bool userInteraction) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    HWND m_hwnd = nullptr;
    winrt::Microsoft::UI::Xaml::Window m_mainWindow{nullptr};

    std::weak_ptr<apc::app::AppController> m_appController;

    std::unique_ptr<TrayIcon> m_trayIcon;
    std::unique_ptr<TrayContextMenu> m_contextMenu;
    winrt::Microsoft::UI::Xaml::Controls::Flyout m_pickerFlyout{nullptr};
    winrt::AudioPlaybackConnector2::DevicePickerView m_devicePickerView{nullptr};

    ShowHelpCallback m_showHelpCallback;
    bool m_openSettingsAfterPickerClosed = false;
    ExitCallback m_exitCallback;
    ResourceStateChangedCallback m_resourceStateChangedCallback;

    // Tray presentation and timers share the window UI thread; no separate status cache.
    static constexpr UINT_PTR c_timerAnimation = 0x41504332;
    static constexpr UINT_PTR c_timerTransientTrayError = 0x41504333;
    static constexpr UINT c_transientTrayErrorMs = 3000;
    std::chrono::steady_clock::time_point m_trayErrorUntil{};
    std::wstring m_transientTrayErrorTooltip;
    bool m_connectingAnimationTimerActive = false;

    UINT m_trayCallbackMsg = WM_APP + 1;
    Theme m_theme = Theme::Dark;
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
    std::atomic_uint64_t m_pickerOpenedGeneration{0};
    std::mutex m_pickerOpenedMutex;
    std::condition_variable_any m_pickerOpenedChanged;

    bool m_devicePickerPreloadInitialized = false;
    bool m_releaseDevicePickerPending = false;
    bool m_pickerRefreshPending = false;
    bool m_useSystemBackdropEffects = true;
    std::atomic_bool m_isTearingDown = false;
};
