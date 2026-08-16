#include <pch.h>
#include <services/TrayController.hpp>
#include <core/Settings.hpp>
#include <core/TrayTooltipBuilder.hpp>
#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <core/ThemeHelper.hpp>
#include <ui/FlyoutPresenterStyle.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

TrayController::TrayController() = default;

TrayController::~TrayController() {
    Teardown();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Lifecycle /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TrayController::Initialize(HWND hwnd, winrt::Microsoft::UI::Xaml::Window mainWindow) {
    m_isTearingDown.store(false);
    m_hwnd = hwnd;
    m_mainWindow = mainWindow;
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_lastLeftClickTick = 0;
    m_lastRightClickTick = 0;
    m_lastLeftDoubleClickTick = 0;
    m_lastPickerClosedOverTrayIconTick = 0;
    m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;
    m_releaseDevicePickerPending = false;
    m_pickerRefreshPending = false;

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
        m_contextMenu->Initialize(
            root,
            [weak]() {
                if (auto self = weak.lock(); self && !self->m_isTearingDown.load() && self->m_showSettingsCallback)
                    self->m_showSettingsCallback();
            },
            [weak]() {
                if (auto self = weak.lock(); self && !self->m_isTearingDown.load()) self->LaunchBluetoothSettings();
            },
            [weak]() {
                if (auto self = weak.lock(); self && !self->m_isTearingDown.load() && self->m_exitCallback)
                    self->m_exitCallback();
            },
            [weak]() {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                    SetWindowPos(self->m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            });
        DebugTrace(L"[TrayController] TrayContextMenu initialized");
    }
}

void TrayController::SetDeviceManager(std::shared_ptr<DeviceManager> deviceManager) {
    m_deviceManager = std::move(deviceManager);
}

void TrayController::SetSettings(std::shared_ptr<Settings> settings) {
    m_settings = std::move(settings);
    if (m_settings) {
        auto locked = m_settings->LockSharedData();
        SetSystemBackdropEffectsEnabled(locked->UseSystemBackdropEffects);
    }
}

void TrayController::ApplyLanguage() {
    if (m_contextMenu) m_contextMenu->ApplyLanguage();
    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->ApplyLanguage();
    }
}

void TrayController::SetSystemBackdropEffectsEnabled(bool enabled) noexcept try {
    m_useSystemBackdropEffects = enabled;
    if (m_contextMenu) m_contextMenu->SetSystemBackdropEffectsEnabled(enabled);
    if (m_pickerFlyout) {
        if (enabled) {
            m_pickerFlyout.SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
        } else {
            m_pickerFlyout.SystemBackdrop(nullptr);
        }
        if (m_pickerFlyout.Content()) {
            apc::ui::ApplyFlyoutPresenterStyle(
                m_pickerFlyout.Content().as<winrt::Microsoft::UI::Xaml::DependencyObject>(), enabled);
        }
    }
} catch (...) {
    util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to apply backdrop setting");
}

void TrayController::Teardown() noexcept try {
    m_isTearingDown.store(true);

    // Marshal to UI thread if necessary so XAML objects are destroyed on the correct thread.
    if (m_mainWindow) {
        try {
            if (auto dispatcher = m_mainWindow.DispatcherQueue()) {
                if (!dispatcher.HasThreadAccess()) {
                    bool enqueued = false;
                    if (auto self = weak_from_this().lock()) {
                        enqueued = dispatcher.TryEnqueue([self = std::move(self)]() noexcept { self->Teardown(); });
                    }
                    if (enqueued) return;

                    DebugTrace(
                        L"[TrayController] UI dispatcher unavailable during teardown; continuing best-effort cleanup");
                }
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: failed to marshal teardown to UI thread", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: failed to marshal teardown to UI thread", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to marshal teardown to UI thread");
        }
    }

    if (m_pickerFlyout) {
        try {
            m_pickerFlyout.Hide();
        } catch (...) {
            DebugTrace(L"[TrayController] ERROR: failed to hide picker flyout during teardown");
        }
    }
    if (m_devicePickerView) {
        try {
            auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
            impl->PrepareForRelease();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: failed to prepare picker for teardown", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: failed to prepare picker for teardown", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to prepare picker for teardown");
        }
    }
    if (m_themeChangedToken) {
        ThemeHelper::RemoveThemeChangedHandler(m_themeChangedToken);
        m_themeChangedToken = 0;
    }
    m_showSettingsCallback = nullptr;
    m_exitCallback = nullptr;
    m_connectCallback = nullptr;
    m_disconnectCallback = nullptr;
    m_reconnectCallback = nullptr;
    m_disconnectAllCallback = nullptr;
    m_reconnectAllCallback = nullptr;
    m_toggleDeviceCallback = nullptr;
    m_resourceStateChangedCallback = nullptr;
    if (m_trayIcon) {
        m_trayIcon->Remove();
        m_trayIcon.reset();
    }
    m_contextMenu.reset();
    m_devicePickerView = nullptr;
    m_pickerFlyout = nullptr;
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_hwnd = nullptr;
    m_devicePickerPreloadInitialized = false;
    m_releaseDevicePickerPending = false;
    m_pickerRefreshPending = false;
    m_lastLeftClickTick = 0;
    m_lastRightClickTick = 0;
    m_lastLeftDoubleClickTick = 0;
    m_lastPickerClosedOverTrayIconTick = 0;
    m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;
} catch (winrt::hresult_error const& ex) {
    util::DebugTraceException(L"[TrayController] ERROR: unexpected teardown failure", ex);
} catch (std::exception const& ex) {
    util::DebugTraceException(L"[TrayController] ERROR: unexpected teardown failure", ex);
} catch (...) {
    util::DebugTraceUnknownException(L"[TrayController] ERROR: unexpected teardown failure");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Callbacks /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TrayController::SetCallbacks(ShowSettingsCallback showSettings,
                                  ExitCallback exit,
                                  DeviceActionCallback connect,
                                  DeviceActionCallback disconnect,
                                  DeviceActionCallback reconnect,
                                  ToggleDeviceCallback toggleDevice,
                                  BulkDeviceActionCallback disconnectAll,
                                  BulkDeviceActionCallback reconnectAll) {
    m_showSettingsCallback = std::move(showSettings);
    m_exitCallback = std::move(exit);
    m_connectCallback = std::move(connect);
    m_disconnectCallback = std::move(disconnect);
    m_reconnectCallback = std::move(reconnect);
    m_disconnectAllCallback = std::move(disconnectAll);
    m_reconnectAllCallback = std::move(reconnectAll);
    m_toggleDeviceCallback = std::move(toggleDevice);
}

void TrayController::SetResourceStateChangedCallback(ResourceStateChangedCallback callback) {
    m_resourceStateChangedCallback = std::move(callback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Actions ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    auto resetTopmost = wil::scope_exit([this]() noexcept {
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    });
    try {
        if (m_contextMenu->ShowAt(point)) resetTopmost.release();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to show context menu", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to show context menu", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to show context menu");
    }
}

bool TrayController::ShowDevicePicker(bool toggleIfOpen) noexcept try {
    if (m_isTearingDown.load()) return false;
    DebugTrace(L"[TrayController] OnTrayIconLeftClick()");

    auto const flyoutState = m_pickerFlyoutState.load();
    if (flyoutState == PickerFlyoutState::Open && m_pickerFlyout) {
        NotifyResourceStateChanged(true);
        if (toggleIfOpen) TryHideDevicePicker();
        return true;
    }

    if (flyoutState != PickerFlyoutState::Closed) {
        DebugTrace(L"[TrayController] Picker flyout state is not closed, ignoring click");
        return false;
    }

    if (m_suppressNextTraySelectAfterPickerClosedOverTrayIcon) {
        constexpr ULONGLONG c_trayLightDismissSuppressMs = 2000;
        auto const elapsedSinceDismiss = GetTickCount64() - m_lastPickerClosedOverTrayIconTick;
        m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;
        if (elapsedSinceDismiss < c_trayLightDismissSuppressMs) {
            DebugTrace(L"[TrayController] Picker flyout reopen suppressed after tray light-dismiss");
            if (toggleIfOpen) return false;
        }
    }

    auto rect = m_trayIcon->GetIconRect();
    if (!rect) {
        DebugTrace(L"[TrayController] ERROR: GetIconRect() returned null");
        return false;
    }

    auto root = m_mainWindow.Content().as<Controls::Grid>();
    if (!root) {
        DebugTrace(L"[TrayController] ERROR: MainWindow.Content() is not a Grid");
        return false;
    }

    if (!EnsureDevicePickerViewCreated()) {
        return false;
    }
    m_pickerRefreshPending = false;
    m_pickerFlyoutState.store(PickerFlyoutState::Opening);
    try {
        m_pickerFlyout = CreatePickerFlyout();
    } catch (winrt::hresult_error const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceException(L"[TrayController] ERROR: failed to create picker flyout", ex);
        return false;
    } catch (std::exception const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceException(L"[TrayController] ERROR: failed to create picker flyout", ex);
        return false;
    } catch (...) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to create picker flyout");
        return false;
    }

    auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
    impl->SetPresentationActive(true);
    auto deactivatePresentation = wil::scope_exit([impl]() noexcept { impl->SetPresentationActive(false); });
    if (impl->LoadDevices()) m_devicePickerPreloadInitialized = true;
    NotifyResourceStateChanged(true);

    POINT pt{rect->left, rect->top};
    ScreenToClient(m_hwnd, &pt);
    auto dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;

    winrt::Windows::Foundation::Point point(
        static_cast<float>(pt.x) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi),
        static_cast<float>(pt.y) * USER_DEFAULT_SCREEN_DPI / static_cast<float>(dpi));

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    auto resetTopmost = wil::scope_exit([this]() noexcept {
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    });
    SetForegroundWindow(m_hwnd);

    Controls::Primitives::FlyoutShowOptions options;
    options.Position(point);
    try {
        m_pickerFlyout.ShowAt(root, options);
        resetTopmost.release();
        deactivatePresentation.release();
        return true;
    } catch (winrt::hresult_error const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceException(L"[TrayController] ERROR: failed to show picker flyout", ex);
        return false;
    } catch (std::exception const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceException(L"[TrayController] ERROR: failed to show picker flyout", ex);
        return false;
    } catch (...) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        m_pickerRefreshPending = false;
        NotifyResourceStateChanged(false);
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to show picker flyout");
        return false;
    }
} catch (winrt::hresult_error const& ex) {
    TryHideDevicePicker();
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_pickerFlyout = nullptr;
    m_pickerRefreshPending = false;
    NotifyResourceStateChanged(false);
    util::DebugTraceException(L"[TrayController] ERROR: unexpected device picker show failure", ex);
    return false;
} catch (std::exception const& ex) {
    TryHideDevicePicker();
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_pickerFlyout = nullptr;
    m_pickerRefreshPending = false;
    NotifyResourceStateChanged(false);
    util::DebugTraceException(L"[TrayController] ERROR: unexpected device picker show failure", ex);
    return false;
} catch (...) {
    TryHideDevicePicker();
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_pickerFlyout = nullptr;
    m_pickerRefreshPending = false;
    NotifyResourceStateChanged(false);
    util::DebugTraceUnknownException(L"[TrayController] ERROR: unexpected device picker show failure");
    return false;
}

void TrayController::UpdateTooltip(std::wstring_view text) {
    if (m_trayIcon) {
        m_trayIcon->SetTooltip(text);
    }
}

void TrayController::UpdateTooltipFromConnections(std::vector<DeviceTrayPresentationItem> const& connected) {
    if (!m_trayIcon) return;
    auto const appName = std::wstring(_("AppName"));
    auto const redactedDeviceName = std::wstring(_("Privacy_RedactedDevice"));
    if (!m_settings) {
        m_trayIcon->SetTooltip(apc::tray::BuildTooltip(appName, redactedDeviceName, connected, {}, false));
        return;
    }

    std::wstring tooltip;
    {
        auto locked = m_settings->LockSharedData();
        tooltip = apc::tray::BuildTooltip(
            appName, redactedDeviceName, connected, locked->Devices, locked->PrivacyModeEnabled);
    }
    m_trayIcon->SetTooltip(tooltip);
}

bool TrayController::RefreshDevicePickerState() noexcept {
    if (m_isTearingDown.load()) {
        return true;
    }
    if (!m_devicePickerView) {
        return true;
    }
    auto flyoutState = m_pickerFlyoutState.load();
    if (flyoutState == PickerFlyoutState::Opening) {
        m_pickerRefreshPending = true;
        return true;
    }
    if (flyoutState != PickerFlyoutState::Open) {
        return true;
    }
    if (!m_pickerFlyout || !m_pickerFlyout.Content()) {
        return false;
    }
    try {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->RefreshDeviceStates();
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to refresh picker device state", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to refresh picker device state", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to refresh picker device state");
    }
    return false;
}

bool TrayController::InvalidateDevicePickerInventory() noexcept {
    if (m_isTearingDown.load() || !m_devicePickerView) return true;
    try {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        return impl->InvalidateDeviceInventory();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to invalidate picker inventory", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to invalidate picker inventory", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to invalidate picker inventory");
    }
    return false;
}

void TrayController::OnThemeChanged() {
    if (m_trayIcon) {
        m_trayIcon->UpdateTheme();
    }
}

bool TrayController::AdvanceConnectingFrame() noexcept {
    if (!m_trayIcon) return false;
    try {
        return m_trayIcon->AdvanceConnectingFrame();
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: connecting frame update failed");
        return false;
    }
}

bool TrayController::ApplyPendingTrayUpdates() noexcept {
    if (!m_trayIcon) return false;
    try {
        return m_trayIcon->ApplyPendingUpdates();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: pending tray update failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: pending tray update failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: pending tray update failed");
    }
    return false;
}

void TrayController::Reregister() {
    if (m_trayIcon) {
        m_trayIcon->Reregister();
    }
}

void TrayController::SetState(TrayIconState state) {
    if (m_trayIcon) {
        DebugTrace(L"[TrayController] SetState requested={0}", TrayIconStateToString(state));
        m_trayIcon->SetState(state);
    }
}

util::SettingsWindowPlacement TrayController::GetSettingsWindowPlacement() const {
    return CalculateSettingsWindowPlacement();
}

void TrayController::HandleTrayMessage([[maybe_unused]] WPARAM wParam, LPARAM lParam) noexcept try {
    if (m_isTearingDown.load()) return;

    auto dispatcher = m_mainWindow ? m_mainWindow.DispatcherQueue() : nullptr;
    if (dispatcher && !dispatcher.HasThreadAccess()) {
        auto weak = weak_from_this();
        bool enqueued = dispatcher.TryEnqueue([weak, wParam, lParam]() noexcept {
            if (auto self = weak.lock()) {
                self->HandleTrayMessage(wParam, lParam);
            }
        });
        if (!enqueued) {
            DebugTrace(L"[TrayController] ERROR: failed to marshal tray message to UI thread");
        }
        return;
    }

    auto loword = LOWORD(lParam);

    constexpr ULONGLONG c_clickDebounceMs = 200;
    constexpr ULONGLONG c_doubleClickSuppressMs = 450;

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
            if (GetTickCount64() - m_lastLeftDoubleClickTick < c_doubleClickSuppressMs) {
                break;
            }
            if (shouldProcess(m_lastLeftClickTick, c_clickDebounceMs)) {
                (void)ShowDevicePicker();
            }
            break;

        case WM_LBUTTONDBLCLK:
            m_lastLeftDoubleClickTick = GetTickCount64();
            if (m_pickerFlyout && m_pickerFlyoutState.load() != PickerFlyoutState::Closed) {
                TryHideDevicePicker();
            }
            OnTrayIconDoubleClick();
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
        case NIN_BALLOONUSERCLICK: break;

        default: DebugTrace(L"[TrayController] Unhandled tray message: 0x{0:X}", loword); break;
    }
} catch (winrt::hresult_error const& ex) {
    util::DebugTraceException(L"[TrayController] ERROR: tray message handling failed", ex);
} catch (std::exception const& ex) {
    util::DebugTraceException(L"[TrayController] ERROR: tray message handling failed", ex);
} catch (...) {
    util::DebugTraceUnknownException(L"[TrayController] ERROR: tray message handling failed");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool TrayController::EnsureDevicePickerViewCreated() noexcept {
    if (m_devicePickerView) {
        return true;
    }

    try {
        auto root = m_mainWindow.Content().as<Controls::Grid>();
        if (!root) {
            DebugTrace(L"[TrayController] ERROR: EnsureDevicePickerViewCreated failed, Content() is not a Grid");
            return false;
        }
        if (!root.XamlRoot()) {
            DebugTrace(L"[TrayController] ERROR: EnsureDevicePickerViewCreated failed, XamlRoot() is null");
            return false;
        }

        auto pickerView = winrt::AudioPlaybackConnector2::DevicePickerView();
        auto impl = pickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        auto weak = weak_from_this();
        impl->Initialize(
            m_deviceManager,
            m_settings,
            [weak]() {
                auto self = weak.lock();
                if (self && !self->m_isTearingDown.load()) self->TryHideDevicePicker();
            },
            [weak](winrt::hstring id) {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                DebugTrace(L"[TrayController] User selected device: {0}", std::wstring(id));
                if (self->m_connectCallback) self->m_connectCallback(id);
                self->TryHideDevicePicker();
            },
            [weak](winrt::hstring id) {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                DebugTrace(L"[TrayController] User disconnected device: {0}", std::wstring(id));
                self->TryHideDevicePicker();
                if (self->m_disconnectCallback) self->m_disconnectCallback(id);
            },
            [weak](winrt::hstring id) {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                DebugTrace(L"[TrayController] User reconnected device: {0}", std::wstring(id));
                self->TryHideDevicePicker();
                try {
                    auto dispatcher = self->m_mainWindow ? self->m_mainWindow.DispatcherQueue() : nullptr;
                    if (dispatcher) {
                        auto weakSelf = weak;
                        bool queued = dispatcher.TryEnqueue([weakSelf, id]() {
                            if (auto queuedSelf = weakSelf.lock();
                                queuedSelf && !queuedSelf->m_isTearingDown.load() && queuedSelf->m_reconnectCallback) {
                                queuedSelf->m_reconnectCallback(id);
                            }
                        });
                        if (queued) return;
                    }
                } catch (...) {
                    util::DebugTraceUnknownException(L"[TrayController] failed to queue reconnect callback");
                }
                if (self->m_reconnectCallback) self->m_reconnectCallback(id);
            },
            [weak]() {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                DebugTrace(L"[TrayController] User disconnected all devices");
                if (self->m_disconnectAllCallback) self->m_disconnectAllCallback();
            },
            [weak]() {
                auto self = weak.lock();
                if (!self || self->m_isTearingDown.load()) return;
                DebugTrace(L"[TrayController] User reconnected all devices");
                if (self->m_reconnectAllCallback) self->m_reconnectAllCallback();
            });
        m_devicePickerView = std::move(pickerView);
        m_releaseDevicePickerPending = false;
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to create device picker", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to create device picker", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to create device picker");
    }
    m_devicePickerView = nullptr;
    return false;
}

void TrayController::PreloadDevicePicker() noexcept {
    try {
        if (m_isTearingDown.load() || m_devicePickerPreloadInitialized) {
            return;
        }

        if (!EnsureDevicePickerViewCreated()) {
            return;
        }

        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        if (impl->LoadDevices()) {
            m_devicePickerPreloadInitialized = true;
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to preload device picker", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to preload device picker", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to preload device picker");
    }
}

bool TrayController::IsDevicePickerLoaded() const noexcept {
    return m_devicePickerView != nullptr;
}

bool TrayController::IsDevicePickerPreloadInitialized() const noexcept {
    return m_devicePickerView != nullptr && m_devicePickerPreloadInitialized;
}

bool TrayController::IsDevicePickerVisibleOrTransitioning() const noexcept {
    return m_pickerFlyoutState.load() != PickerFlyoutState::Closed;
}

uint64_t TrayController::DevicePickerOpenedGeneration() const noexcept {
    return m_pickerOpenedGeneration.load();
}

void TrayController::ReleaseDevicePicker() noexcept {
    try {
        if (m_isTearingDown.load() || !m_devicePickerView) {
            return;
        }

        auto dispatcher = m_mainWindow ? m_mainWindow.DispatcherQueue() : nullptr;
        if (!dispatcher || !dispatcher.HasThreadAccess()) {
            DebugTrace(L"[TrayController] ERROR: ReleaseDevicePicker must run on the UI thread");
            return;
        }

        auto const flyoutState = m_pickerFlyoutState.load();
        if (flyoutState != PickerFlyoutState::Closed) {
            m_releaseDevicePickerPending = true;
            if (m_pickerFlyout && flyoutState != PickerFlyoutState::Closing) {
                TryHideDevicePicker();
            }
            return;
        }

        ReleaseDevicePickerOnUIThread();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to request device picker release", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to request device picker release", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to request device picker release");
    }
}

void TrayController::TryHideDevicePicker() noexcept {
    if (!m_pickerFlyout) return;

    auto const previousState = m_pickerFlyoutState.load();
    if (previousState == PickerFlyoutState::Closed || previousState == PickerFlyoutState::Closing) return;

    auto restoreVisiblePresentation = [&]() noexcept {
        m_pickerFlyoutState.store(previousState);
        try {
            if (m_devicePickerView) {
                auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
                impl->SetPresentationActive(true);
                if (!impl->LoadDevices()) m_pickerRefreshPending = true;
            }
        } catch (...) {
            m_pickerRefreshPending = true;
        }
        NotifyResourceStateChanged(true);
    };

    m_pickerFlyoutState.store(PickerFlyoutState::Closing);
    try {
        m_pickerFlyout.Hide();
    } catch (winrt::hresult_error const& ex) {
        restoreVisiblePresentation();
        util::DebugTraceException(L"[TrayController] ERROR: failed to hide device picker", ex);
    } catch (std::exception const& ex) {
        restoreVisiblePresentation();
        util::DebugTraceException(L"[TrayController] ERROR: failed to hide device picker", ex);
    } catch (...) {
        restoreVisiblePresentation();
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to hide device picker");
    }
}

void TrayController::ReleaseDevicePickerOnUIThread() noexcept {
    if (!m_devicePickerView) {
        m_devicePickerPreloadInitialized = false;
        m_releaseDevicePickerPending = false;
        return;
    }

    try {
        auto dispatcher = m_mainWindow ? m_mainWindow.DispatcherQueue() : nullptr;
        if (!dispatcher || !dispatcher.HasThreadAccess()) {
            DebugTrace(L"[TrayController] ERROR: picker release attempted outside the UI thread");
            return;
        }
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->PrepareForRelease();
        m_devicePickerView = nullptr;
        m_devicePickerPreloadInitialized = false;
        m_releaseDevicePickerPending = false;
        m_pickerRefreshPending = false;
        DebugTrace(L"[TrayController] Device picker released");
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to release device picker", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to release device picker", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to release device picker");
    }
}

Controls::Flyout TrayController::CreatePickerFlyout() {
    if (!m_devicePickerView) {
        throw winrt::hresult_invalid_argument(L"Cannot create a picker flyout without content");
    }
    Controls::Flyout flyout;
    flyout.ShouldConstrainToRootBounds(false);
    if (m_useSystemBackdropEffects) {
        flyout.SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
    }
    flyout.Content(m_devicePickerView);

    auto weak = weak_from_this();
    flyout.Opened([weak](auto const& sender, auto&) noexcept {
        try {
            auto self = weak.lock();
            auto openedFlyout = sender.template try_as<Controls::Flyout>();
            if (self && !self->m_isTearingDown.load() && openedFlyout && self->m_pickerFlyout == openedFlyout) {
                self->m_pickerFlyoutState.store(PickerFlyoutState::Open);
                self->m_pickerOpenedGeneration.fetch_add(1);
                if (std::exchange(self->m_pickerRefreshPending, false)) {
                    if (!self->RefreshDevicePickerState()) self->m_pickerRefreshPending = true;
                }
                apc::ui::ApplyFlyoutPresenterStyle(
                    self->m_pickerFlyout.Content().as<winrt::Microsoft::UI::Xaml::DependencyObject>(),
                    self->m_useSystemBackdropEffects);
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout opened handler failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout opened handler failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: Picker flyout opened handler failed");
        }
    });

    flyout.Closing([weak](auto const& sender, auto&) noexcept {
        try {
            auto self = weak.lock();
            auto closingFlyout = sender.template try_as<Controls::Flyout>();
            if (!self || self->m_isTearingDown.load() || !closingFlyout || self->m_pickerFlyout != closingFlyout) {
                return;
            }
            self->m_pickerFlyoutState.store(PickerFlyoutState::Closing);
            if (self->m_devicePickerView) {
                auto impl =
                    self->m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
                impl->SetPresentationActive(false);
                impl->CancelLoadDevices();
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closing handler failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closing handler failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: Picker flyout closing handler failed");
        }
    });

    flyout.Closed([weak](auto const& sender, auto&) noexcept {
        try {
            auto self = weak.lock();
            auto closedFlyout = sender.template try_as<Controls::Flyout>();
            if (!self || self->m_isTearingDown.load() || !closedFlyout || self->m_pickerFlyout != closedFlyout) {
                return;
            }
            DebugTrace(L"[TrayController] Picker flyout closed");
            auto const leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (leftButtonDown && self->IsCursorOverTrayIcon()) {
                self->m_lastPickerClosedOverTrayIconTick = GetTickCount64();
                self->m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = true;
            }
            if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                SetWindowPos(self->m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            self->m_pickerFlyoutState.store(PickerFlyoutState::Closed);
            self->m_pickerFlyout = nullptr;
            self->m_pickerRefreshPending = false;
            self->NotifyResourceStateChanged(false);
            if (self->m_releaseDevicePickerPending) {
                self->ReleaseDevicePickerOnUIThread();
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closed handler failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closed handler failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: Picker flyout closed handler failed");
        }
    });

    return flyout;
}

void TrayController::NotifyResourceStateChanged(bool userInteraction) noexcept {
    if (!m_resourceStateChangedCallback) return;
    try {
        m_resourceStateChangedCallback(userInteraction);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] resource-state callback failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] resource-state callback failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] resource-state callback failed");
    }
}

util::SettingsWindowPlacement TrayController::CalculateSettingsWindowPlacement() const {
    std::optional<RECT> anchorRect;
    if (m_trayIcon) {
        anchorRect = m_trayIcon->GetIconRect();
    }
    return util::CalculateSettingsWindowPlacement(anchorRect);
}

bool TrayController::IsCursorOverTrayIcon() const {
    if (!m_trayIcon) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;

    auto rect = m_trayIcon->GetIconRect();
    if (!rect) return false;

    return PtInRect(&*rect, cursor) != FALSE;
}

void TrayController::OnTrayIconDoubleClick() {
    if (m_isTearingDown.load()) return;
    DebugTrace(L"[TrayController] OnTrayIconDoubleClick()");
    if (m_toggleDeviceCallback) {
        m_toggleDeviceCallback();
    }
}

void TrayController::LaunchBluetoothSettings() {
    try {
        auto op =
            winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:bluetooth"));
        op.Completed([](auto const& sender, auto const&) noexcept {
            try {
                if (!sender.GetResults()) {
                    DebugTrace(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed");
                }
            } catch (winrt::hresult_error const& ex) {
                util::DebugTraceException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed", ex);
            } catch (std::exception const& ex) {
                util::DebugTraceException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed", ex);
            } catch (...) {
                util::DebugTraceUnknownException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed");
            }
        });
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] LaunchUriAsync(ms-settings:bluetooth) failed");
    }
}
