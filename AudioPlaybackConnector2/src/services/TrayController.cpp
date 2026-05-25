#include <pch.h>
#include <services/TrayController.hpp>
#include <core/DeviceManager.hpp>
#include <core/ThemeHelper.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

TrayController::TrayController() = default;

TrayController::~TrayController() {
    Teardown();
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Lifecycle //////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayController::Initialize(HWND hwnd, winrt::Microsoft::UI::Xaml::Window mainWindow) {
    m_isTearingDown.store(false);
    m_hwnd = hwnd;
    m_mainWindow = mainWindow;
    m_lastLeftClickTick = 0;
    m_lastRightClickTick = 0;

    m_trayIcon = std::make_unique<TrayIcon>();
    m_trayIcon->Initialize(m_hwnd, m_trayCallbackMsg);
    DebugTrace(L"[TrayController] TrayIcon initialized");

    auto weak = weak_from_this();
    m_themeChangedToken = ThemeHelper::AddThemeChangedHandler([weak]() {
        auto self = weak.lock();
        if (!self || self->m_isTearingDown.load() || !self->m_trayIcon) return;
        DebugTrace(L"[TrayController] System theme changed");
        self->m_trayIcon->UpdateTheme();
    });

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (root && root.XamlRoot()) {
        m_contextMenu = std::make_unique<TrayContextMenu>();
        m_contextMenu->Initialize(root, [weak]() {
            if (auto self = weak.lock(); self && !self->m_isTearingDown.load() && self->m_showSettingsCallback) self->m_showSettingsCallback(); }, [weak]() {
            if (auto self = weak.lock(); self && !self->m_isTearingDown.load()) self->LaunchBluetoothSettings(); }, [weak]() {
            if (auto self = weak.lock(); self && !self->m_isTearingDown.load() && self->m_exitCallback) self->m_exitCallback(); }, [weak]() {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                SetWindowPos(self->m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } });
        DebugTrace(L"[TrayController] TrayContextMenu initialized");
    }
}

void TrayController::SetDeviceManager(std::shared_ptr<DeviceManager> deviceManager) {
    m_deviceManager = std::move(deviceManager);
}

void TrayController::Teardown() noexcept {
    m_isTearingDown.store(true);
    if (m_themeChangedToken) {
        ThemeHelper::RemoveThemeChangedHandler(m_themeChangedToken);
        m_themeChangedToken = 0;
    }
    m_showSettingsCallback = nullptr;
    m_exitCallback = nullptr;
    m_connectCallback = nullptr;
    m_disconnectCallback = nullptr;
    m_reconnectCallback = nullptr;
    if (m_trayIcon) {
        m_trayIcon->Remove();
        m_trayIcon.reset();
    }
    m_contextMenu.reset();
    m_devicePickerView = nullptr;
    m_pickerFlyout = nullptr;
    m_hwnd = nullptr;
    m_devicePickerPreloaded = false;
    m_lastLeftClickTick = 0;
    m_lastRightClickTick = 0;
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Callbacks //////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayController::SetCallbacks(ShowSettingsCallback showSettings, ExitCallback exit, DeviceActionCallback connect, DeviceActionCallback disconnect, DeviceActionCallback reconnect) {
    m_showSettingsCallback = std::move(showSettings);
    m_exitCallback = std::move(exit);
    m_connectCallback = std::move(connect);
    m_disconnectCallback = std::move(disconnect);
    m_reconnectCallback = std::move(reconnect);
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Actions ////////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayController::ShowTrayMenu() {
    if (m_isTearingDown.load()) return;
    DebugTrace(L"[TrayController] OnTrayIconRightClick()");
    if (!m_contextMenu) {
        DebugTrace(L"[TrayController] ERROR: m_contextMenu is null");
        return;
    }

    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(m_hwnd, &pt);

    auto dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    SetForegroundWindow(m_hwnd);

    winrt::Windows::Foundation::Point point(
        static_cast<float>(pt.x) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi),
        static_cast<float>(pt.y) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi));

    DebugTrace(L"[TrayController] ContextMenu showing at ({0}, {1}) with DPI={2}", point.X, point.Y, dpi);

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    m_contextMenu->ShowAt(point);
}

void TrayController::ShowDevicePicker() {
    if (m_isTearingDown.load()) return;
    DebugTrace(L"[TrayController] OnTrayIconLeftClick()");

    auto rect = m_trayIcon->GetIconRect();
    if (!rect) {
        DebugTrace(L"[TrayController] ERROR: GetIconRect() returned null");
        return;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[TrayController] ERROR: MainWindow.Content() is not a Grid");
        return;
    }

    if (m_pickerFlyout && m_pickerFlyout.IsOpen()) {
        DebugTrace(L"[TrayController] Picker flyout already open, ignoring second click");
        return;
    }

    EnsureDevicePickerViewCreated();

    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->LoadDevices();
    }

    m_pickerFlyout = CreatePickerFlyout();

    POINT pt{rect->left, rect->top};
    ScreenToClient(m_hwnd, &pt);
    auto dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    winrt::Windows::Foundation::Point point(
        static_cast<float>(pt.x) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi),
        static_cast<float>(pt.y) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi));

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(m_hwnd);

    Controls::Primitives::FlyoutShowOptions options;
    options.Position(point);
    m_pickerFlyout.ShowAt(root, options);
}

void TrayController::UpdateTooltip(std::wstring_view text) {
    if (m_trayIcon) {
        m_trayIcon->SetTooltip(text);
    }
}

void TrayController::ShowNotification(std::wstring const& title, std::wstring const& body, TrayNotificationType type) {
    if (m_trayIcon) {
        m_trayIcon->ShowNotification(title, body, type);
    }
}

void TrayController::UpdateTooltipFromConnections() {
    if (!m_trayIcon || !m_deviceManager) return;
    auto connected = m_deviceManager->GetConnectedDevices();
    if (connected.empty()) {
        m_trayIcon->SetTooltip(_("AppName"));
    } else {
        std::wstring tip = std::wstring(_("AppName")) + L"\n";
        for (const auto& c : connected) {
            tip += c.Device.Name();
            tip += L"\n";
        }
        m_trayIcon->SetTooltip(tip);
    }
}

void TrayController::OnThemeChanged() {
    if (m_trayIcon) {
        m_trayIcon->UpdateTheme();
    }
}

void TrayController::ToggleConnectingFrame() {
    if (m_trayIcon) {
        m_trayIcon->ToggleConnectingFrame();
    }
}

void TrayController::Reregister() {
    if (m_trayIcon) {
        m_trayIcon->Reregister();
    }
}

void TrayController::SetState(TrayIconState state) {
    if (m_trayIcon) {
        m_trayIcon->SetState(state);
    }
}

std::optional<POINT> TrayController::GetSettingsWindowPosition() const {
    return CalculateSettingsWindowPosition();
}

void TrayController::HandleTrayMessage([[maybe_unused]] WPARAM wParam, LPARAM lParam) {
    if (m_isTearingDown.load()) return;
    auto loword = LOWORD(lParam);

    constexpr ULONGLONG c_clickDebounceMs = 200;

    auto shouldProcess = [](ULONGLONG& lastTick, ULONGLONG debounceMs) {
        auto now = GetTickCount64();
        if (now - lastTick < debounceMs) {
            return false;
        }
        lastTick = now;
        return true;
    };

    switch (loword) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (shouldProcess(m_lastLeftClickTick, c_clickDebounceMs)) {
                ShowDevicePicker();
            }
            break;

        case WM_CONTEXTMENU:
            if (shouldProcess(m_lastRightClickTick, c_clickDebounceMs)) {
                ShowTrayMenu();
            }
            break;

        case WM_RBUTTONUP:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MOUSEMOVE:
        case NIN_BALLOONSHOW:
        case NIN_BALLOONHIDE:
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONUSERCLICK:
            break;

        default:
            DebugTrace(L"[TrayController] Unhandled tray message: 0x{0:X}", loword);
            break;
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayController::EnsureDevicePickerViewCreated() {
    if (m_devicePickerView) {
        return;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[TrayController] ERROR: EnsureDevicePickerViewCreated failed, Content() is not a Grid");
        return;
    }
    if (!root.XamlRoot()) {
        DebugTrace(L"[TrayController] ERROR: EnsureDevicePickerViewCreated failed, XamlRoot() is null");
        return;
    }

    m_devicePickerView = winrt::AudioPlaybackConnector2::DevicePickerView();
    auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
    auto weak = weak_from_this();
    impl->Initialize(m_deviceManager, [weak]() {
            auto self = weak.lock();
            if (self && !self->m_isTearingDown.load() && self->m_pickerFlyout) self->m_pickerFlyout.Hide(); }, [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User selected device: {0}", std::wstring(id));
            if (self->m_connectCallback) self->m_connectCallback(id);
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide(); }, [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User disconnected device: {0}", std::wstring(id));
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide();
            if (self->m_disconnectCallback) self->m_disconnectCallback(id); }, [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User reconnected device: {0}", std::wstring(id));
            if (self->m_reconnectCallback) self->m_reconnectCallback(id);
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide(); });
}

void TrayController::PreloadDevicePicker() {
    if (m_devicePickerPreloaded) {
        return;
    }
    m_devicePickerPreloaded = true;

    EnsureDevicePickerViewCreated();

    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->LoadDevices();
    }
}

void TrayController::StripFlyoutPresenterStyle(winrt::Microsoft::UI::Xaml::DependencyObject const& content) {
    try {
        auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(content);
        while (parent) {
            auto presenter = parent.try_as<Controls::FlyoutPresenter>();
            if (presenter) {
                presenter.Background(nullptr);
                presenter.BorderBrush(nullptr);
                presenter.BorderThickness({0});
                presenter.Padding({0});
                presenter.MinWidth(0);
                presenter.MinHeight(0);
                break;
            }
            parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(parent);
        }
    } catch (...) {
    }
}

Controls::Flyout TrayController::CreatePickerFlyout() {
    Controls::Flyout flyout;
    flyout.ShouldConstrainToRootBounds(false);
    flyout.Content(m_devicePickerView);

    auto weak = weak_from_this();
    flyout.Opened([weak](auto&, auto&) {
        auto self = weak.lock();
        if (self && !self->m_isTearingDown.load() && self->m_pickerFlyout) {
            StripFlyoutPresenterStyle(self->m_pickerFlyout.Content().as<winrt::Microsoft::UI::Xaml::DependencyObject>());
        }
    });

    flyout.Closing([weak](auto&, auto&) {
        try {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            if (self->m_devicePickerView) {
                auto impl = self->m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
                impl->CancelLoadDevices();
            }
            if (self->m_pickerFlyout) {
                self->m_pickerFlyout.Content(nullptr);
            }
        } catch (...) {
        }
    });

    flyout.Closed([weak](auto&, auto&) {
        auto self = weak.lock();
        if (!self || self->m_isTearingDown.load()) return;
        DebugTrace(L"[TrayController] Picker flyout closed");
        if (self->m_hwnd && IsWindow(self->m_hwnd)) {
            SetWindowPos(self->m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        self->m_pickerFlyout = nullptr;
    });

    return flyout;
}

std::optional<POINT> TrayController::CalculateSettingsWindowPosition() const {
    if (!m_trayIcon) return std::nullopt;
    auto rect = m_trayIcon->GetIconRect();
    if (!rect) return std::nullopt;

    RECT rcWork{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

    int x = rect->right - c_settingsWindowWidth;
    int y = rect->bottom;

    if (x < rcWork.left) x = rcWork.left;
    if (x + c_settingsWindowWidth > rcWork.right) x = rcWork.right - c_settingsWindowWidth;

    if (y + c_settingsWindowHeight > rcWork.bottom)
        y = rect->top - c_settingsWindowHeight;

    if (y < rcWork.top)
        y = rcWork.bottom - c_settingsWindowHeight;

    return POINT{x, y};
}

void TrayController::LaunchBluetoothSettings() {
    auto op = winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:bluetooth"));
    op.Completed([](auto const& sender, auto const&) {
        if (!sender.GetResults()) {
            DebugTrace(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed");
        }
    });
}
