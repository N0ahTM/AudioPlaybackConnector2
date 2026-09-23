#include <pch.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>
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
    auto addItem = [this, invokeAfterClose](std::string_view key, wchar_t const* glyph, std::function<void()> action) {
        MenuFlyoutItem item;
        item.Text(winrt::hstring(m_strings->Get(key)));
        FontIcon icon;
        icon.Glyph(glyph);
        item.Icon(icon);
        item.Click([action, invokeAfterClose](auto, auto) { invokeAfterClose(action); });
        return item;
    };
    auto settingsItem = addItem("OpenSettings", L"\xE713", onSettings);
    auto helpItem = addItem("Settings_Help", L"\xE897", onHelp);
    auto btItem = addItem("BluetoothSettings", L"\xE702", onBluetooth);

    MenuFlyoutSeparator sep;

    auto exitItem = addItem("Exit", L"\xE8BB", onExit);

    menu.Items().Append(settingsItem);
    menu.Items().Append(helpItem);
    menu.Items().Append(btItem);
    menu.Items().Append(sep);
    menu.Items().Append(exitItem);
    menu.Opened([settingsItem = winrt::make_weak(settingsItem),
                 backdropEffects = m_useSystemBackdropEffects,
                 log = m_log](auto&, auto&) {
        if (auto item = settingsItem.get()) {
            apc::ui::ApplyFlyoutPresenterStyle(item, backdropEffects->load(std::memory_order_relaxed), log);
        }
    });

    m_menu = menu;
    m_settingsItem = settingsItem;
    m_helpItem = helpItem;
    m_bluetoothItem = btItem;
    m_exitItem = exitItem;
}

void TrayContextMenu::ApplyLanguage() {
    if (m_settingsItem) m_settingsItem.Text(winrt::hstring(m_strings->Get("OpenSettings")));
    if (m_helpItem) m_helpItem.Text(winrt::hstring(m_strings->Get("Settings_Help")));
    if (m_bluetoothItem) m_bluetoothItem.Text(winrt::hstring(m_strings->Get("BluetoothSettings")));
    if (m_exitItem) m_exitItem.Text(winrt::hstring(m_strings->Get("Exit")));
}

bool TrayContextMenu::ShowAt(winrt::Windows::Foundation::Point point) {
    if (!m_menu) {
        m_log.Trace(L"[TrayContextMenu] ERROR: m_menu is null");
        return false;
    }
    if (!m_anchor) {
        m_log.Trace(L"[TrayContextMenu] ERROR: m_anchor is null");
        return false;
    }
    if (!m_anchor.XamlRoot()) {
        m_log.Trace(L"[TrayContextMenu] ERROR: m_anchor.XamlRoot() is null");
        return false;
    }
    m_menu.XamlRoot(m_anchor.XamlRoot());
    m_menu.ShowAt(m_anchor, point);
    return true;
}
