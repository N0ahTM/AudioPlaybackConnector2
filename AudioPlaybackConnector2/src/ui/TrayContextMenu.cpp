#include <pch.h>
#include <ui/TrayContextMenu.hpp>
#include <core/StringResources.hpp>
#include <ui/FlyoutPresenterStyle.hpp>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TrayContextMenu::Initialize(winrt::Microsoft::UI::Xaml::FrameworkElement anchor,
                                 std::function<void()> onSettings,
                                 std::function<void()> onHelp,
                                 std::function<void()> onBluetooth,
                                 std::function<void()> onExit,
                                 std::function<void()> onClosed) {
    m_anchor = anchor;

    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml;

    MenuFlyout menu;
    menu.ShouldConstrainToRootBounds(false);

    struct MenuActionState {
        bool Open = false;
        std::function<void()> Pending;
    };
    auto actionState = std::make_shared<MenuActionState>();
    menu.Opened([actionState](auto const&, auto const&) { actionState->Open = true; });
    menu.Closed([actionState, onClosed](auto const&, auto const&) {
        actionState->Open = false;
        if (onClosed) onClosed();
        auto pending = std::exchange(actionState->Pending, nullptr);
        if (pending) pending();
    });
    auto invokeAfterClose = [actionState](std::function<void()> action) {
        if (actionState->Open)
            actionState->Pending = std::move(action);
        else if (action)
            action();
    };
    MenuFlyoutItem settingsItem;
    settingsItem.Text(winrt::hstring(_("OpenSettings")));
    FontIcon settingsIcon;
    settingsIcon.Glyph(L"\xE713");
    settingsItem.Icon(settingsIcon);
    settingsItem.Click([onSettings, invokeAfterClose](auto, auto) { invokeAfterClose(onSettings); });

    MenuFlyoutItem btItem;
    MenuFlyoutItem helpItem;
    helpItem.Text(winrt::hstring(_("Settings_Help")));
    FontIcon helpIcon;
    helpIcon.Glyph(L"\xE897");
    helpItem.Icon(helpIcon);
    helpItem.Click([onHelp, invokeAfterClose](auto, auto) { invokeAfterClose(onHelp); });
    btItem.Text(winrt::hstring(_("BluetoothSettings")));
    FontIcon btIcon;
    btIcon.Glyph(L"\xE702");
    btItem.Icon(btIcon);
    btItem.Click([onBluetooth, invokeAfterClose](auto, auto) { invokeAfterClose(onBluetooth); });

    MenuFlyoutSeparator sep;

    MenuFlyoutItem exitItem;
    exitItem.Text(winrt::hstring(_("Exit")));
    FontIcon exitIcon;
    exitIcon.Glyph(L"\xE8BB");
    exitItem.Icon(exitIcon);
    exitItem.Click([onExit, invokeAfterClose](auto, auto) { invokeAfterClose(onExit); });

    menu.Items().Append(settingsItem);
    menu.Items().Append(helpItem);
    menu.Items().Append(btItem);
    menu.Items().Append(sep);
    menu.Items().Append(exitItem);
    menu.Opened(
        [settingsItem = winrt::make_weak(settingsItem), backdropEffects = m_useSystemBackdropEffects](auto&, auto&) {
            if (auto item = settingsItem.get()) {
                apc::ui::ApplyFlyoutPresenterStyle(item, backdropEffects->load(std::memory_order_relaxed));
            }
        });

    m_menu = menu;
    m_settingsItem = settingsItem;
    m_helpItem = helpItem;
    m_bluetoothItem = btItem;
    m_exitItem = exitItem;
}

void TrayContextMenu::ApplyLanguage() {
    if (m_settingsItem) m_settingsItem.Text(winrt::hstring(_("OpenSettings")));
    if (m_helpItem) m_helpItem.Text(winrt::hstring(_("Settings_Help")));
    if (m_bluetoothItem) m_bluetoothItem.Text(winrt::hstring(_("BluetoothSettings")));
    if (m_exitItem) m_exitItem.Text(winrt::hstring(_("Exit")));
}

void TrayContextMenu::SetSystemBackdropEffectsEnabled(bool enabled) noexcept {
    m_useSystemBackdropEffects->store(enabled, std::memory_order_relaxed);
}

bool TrayContextMenu::ShowAt(winrt::Windows::Foundation::Point point) {
    if (!m_menu) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_menu is null");
        return false;
    }
    if (!m_anchor) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_anchor is null");
        return false;
    }
    if (!m_anchor.XamlRoot()) {
        DebugTrace(L"[TrayContextMenu] ERROR: m_anchor.XamlRoot() is null");
        return false;
    }
    m_menu.XamlRoot(m_anchor.XamlRoot());
    m_menu.ShowAt(m_anchor, point);
    return true;
}
