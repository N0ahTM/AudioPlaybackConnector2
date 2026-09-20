#pragma once

#include <windows.h>
// Win32 macro conflicts with the WinUI animation projection included by Controls.
#undef GetCurrentTime

#include <atomic>
#include <functional>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

#include <core/StringResources.hpp>
#include <memory>

#include <util/Logger.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Context Menu /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class TrayContextMenu {
public:
    explicit TrayContextMenu(util::LogSink log, std::shared_ptr<StringResources const> strings)
        : m_log(std::move(log)), m_strings(std::move(strings)) {}
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Initialize(winrt::Microsoft::UI::Xaml::FrameworkElement anchor,
                    std::function<void()> onSettings,
                    std::function<void()> onHelp,
                    std::function<void()> onBluetooth,
                    std::function<void()> onExit,
                    std::function<void()> onClosed = nullptr);
    [[nodiscard]] bool ShowAt(winrt::Windows::Foundation::Point point);
    void ApplyLanguage();
    void SetSystemBackdropEffectsEnabled(bool enabled) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout m_menu{nullptr};
    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    winrt::Microsoft::UI::Xaml::FrameworkElement m_anchor{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_settingsItem{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_helpItem{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_bluetoothItem{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_exitItem{nullptr};
    std::shared_ptr<std::atomic_bool> m_useSystemBackdropEffects = std::make_shared<std::atomic_bool>(true);
};
