#include <pch.h>
#include <ui/TrayContextMenu.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TrayContextMenu::Initialize(winrt::Microsoft::UI::Xaml::FrameworkElement anchor,
                                 std::function<void()> onSettings,
                                 std::function<void()> onBluetooth,
                                 std::function<void()> onExit,
                                 std::function<void()> onClosed) {
    m_anchor = anchor;

    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml;

    MenuFlyout menu;
    menu.ShouldConstrainToRootBounds(false);

    if (onClosed) {
        menu.Closed([onClosed](auto&, auto&) { onClosed(); });
    }

    MenuFlyoutItem settingsItem;
    settingsItem.Text(winrt::hstring(_("OpenSettings")));
    FontIcon settingsIcon;
    settingsIcon.Glyph(L"\xE713");
    settingsItem.Icon(settingsIcon);
    settingsItem.Click([onSettings](auto, auto) {
        if (onSettings) onSettings();
    });

    MenuFlyoutItem btItem;
    btItem.Text(winrt::hstring(_("BluetoothSettings")));
    FontIcon btIcon;
    btIcon.Glyph(L"\xE702");
    btItem.Icon(btIcon);
    btItem.Click([onBluetooth](auto, auto) {
        if (onBluetooth) onBluetooth();
    });

    MenuFlyoutSeparator sep;

    MenuFlyoutItem exitItem;
    exitItem.Text(winrt::hstring(_("Exit")));
    FontIcon exitIcon;
    exitIcon.Glyph(L"\xE8BB");
    exitItem.Icon(exitIcon);
    exitItem.Click([onExit](auto, auto) {
        if (onExit) onExit();
    });

    menu.Items().Append(settingsItem);
    menu.Items().Append(btItem);
    menu.Items().Append(sep);
    menu.Items().Append(exitItem);

    m_menu = menu;
}

void TrayContextMenu::ShowAt(winrt::Windows::Foundation::Point point) {
    if (!m_menu) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_menu is null");
        return;
    }
    if (!m_anchor) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_anchor is null");
        return;
    }
    if (!m_anchor.XamlRoot()) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_anchor.XamlRoot() is null");
        return;
    }
    m_menu.XamlRoot(m_anchor.XamlRoot());
    m_menu.ShowAt(m_anchor, point);
}
