#pragma once

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Context Menu /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class TrayContextMenu {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Initialize(winrt::Microsoft::UI::Xaml::FrameworkElement anchor,
                    std::function<void()> onSettings,
                    std::function<void()> onBluetooth,
                    std::function<void()> onExit,
                    std::function<void()> onClosed = nullptr);
    void ShowAt(winrt::Windows::Foundation::Point point);
    void ApplyLanguage();
    void SetSystemBackdropEffectsEnabled(bool enabled) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout m_menu{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement m_anchor{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_settingsItem{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_bluetoothItem{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_exitItem{nullptr};
    bool m_useSystemBackdropEffects = true;
};
