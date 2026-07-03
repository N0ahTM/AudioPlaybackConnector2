#include <pch.h>
#include <services/TrayController.hpp>
#include <core/DeviceManager.hpp>
#include <core/ThemeHelper.hpp>
#include <core/StringResources.hpp>
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

void TrayController::Teardown() noexcept {
    m_isTearingDown.store(true);

    // Marshal to UI thread if necessary so XAML objects are destroyed on the correct thread.
    if (m_mainWindow) {
        if (auto dispatcher = m_mainWindow.DispatcherQueue()) {
            if (!dispatcher.HasThreadAccess()) {
                bool enqueued = false;
                try {
                    if (auto self = weak_from_this().lock()) {
                        enqueued = dispatcher.TryEnqueue([self = std::move(self)]() noexcept { self->Teardown(); });
                    }
                } catch (winrt::hresult_error const& ex) {
                    util::DebugTraceException(L"[TrayController] ERROR: failed to marshal teardown to UI thread", ex);
                } catch (std::exception const& ex) {
                    util::DebugTraceException(L"[TrayController] ERROR: failed to marshal teardown to UI thread", ex);
                } catch (...) {
                    util::DebugTraceUnknownException(
                        L"[TrayController] ERROR: failed to marshal teardown to UI thread");
                }
                if (enqueued) return;

                DebugTrace(
                    L"[TrayController] UI dispatcher unavailable during teardown; continuing best-effort cleanup");
            }
        }
    }

    if (m_pickerFlyout) {
        try {
            m_pickerFlyout.Hide();
        } catch (...) {
            DebugTrace(L"[TrayController] ERROR: failed to hide picker flyout during teardown");
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
    if (m_trayIcon) {
        m_trayIcon->Remove();
        m_trayIcon.reset();
    }
    m_contextMenu.reset();
    m_devicePickerView = nullptr;
    m_pickerFlyout = nullptr;
    m_pickerFlyoutState.store(PickerFlyoutState::Closed);
    m_hwnd = nullptr;
    m_devicePickerPreloaded = false;
    m_lastLeftClickTick = 0;
    m_lastRightClickTick = 0;
    m_lastLeftDoubleClickTick = 0;
    m_lastPickerClosedOverTrayIconTick = 0;
    m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;
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
        m_contextMenu->ShowAt(point);
        resetTopmost.release();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to show context menu", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to show context menu", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to show context menu");
    }
}

void TrayController::ShowDevicePicker() {
    if (m_isTearingDown.load()) return;
    DebugTrace(L"[TrayController] OnTrayIconLeftClick()");

    auto const flyoutState = m_pickerFlyoutState.load();
    if (flyoutState == PickerFlyoutState::Open && m_pickerFlyout) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closing);
        m_pickerFlyout.Hide();
        return;
    }

    if (flyoutState != PickerFlyoutState::Closed) {
        DebugTrace(L"[TrayController] Picker flyout state is not closed, ignoring click");
        return;
    }

    if (m_suppressNextTraySelectAfterPickerClosedOverTrayIcon) {
        constexpr ULONGLONG c_trayLightDismissSuppressMs = 2000;
        auto const elapsedSinceDismiss = GetTickCount64() - m_lastPickerClosedOverTrayIconTick;
        m_suppressNextTraySelectAfterPickerClosedOverTrayIcon = false;
        if (elapsedSinceDismiss < c_trayLightDismissSuppressMs) {
            DebugTrace(L"[TrayController] Picker flyout reopen suppressed after tray light-dismiss");
            return;
        }
    }

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

    EnsureDevicePickerViewCreated();
    ReconcileTrayStateFromDeviceSnapshot(L"flyout-open-before-load");

    m_pickerFlyoutState.store(PickerFlyoutState::Opening);
    try {
        m_pickerFlyout = CreatePickerFlyout();
    } catch (winrt::hresult_error const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceException(L"[TrayController] ERROR: failed to create picker flyout", ex);
        return;
    } catch (std::exception const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceException(L"[TrayController] ERROR: failed to create picker flyout", ex);
        return;
    } catch (...) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to create picker flyout");
        return;
    }

    if (m_devicePickerView) {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->LoadDevices();
    }

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
    auto resetTopmost = wil::scope_exit([this]() noexcept {
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    });
    try {
        m_pickerFlyout.ShowAt(root, options);
        resetTopmost.release();
    } catch (winrt::hresult_error const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceException(L"[TrayController] ERROR: failed to show picker flyout", ex);
    } catch (std::exception const& ex) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceException(L"[TrayController] ERROR: failed to show picker flyout", ex);
    } catch (...) {
        m_pickerFlyoutState.store(PickerFlyoutState::Closed);
        m_pickerFlyout = nullptr;
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to show picker flyout");
    }
}

void TrayController::UpdateTooltip(std::wstring_view text) {
    if (m_trayIcon) {
        m_trayIcon->SetTooltip(text);
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
            auto const& label = !c.Name.empty() ? c.Name : c.Id;
            tip += label;
            tip += L"\n";
        }
        m_trayIcon->SetTooltip(tip);
    }
}

void TrayController::RefreshDevicePickerState() {
    if (m_isTearingDown.load()) {
        return;
    }
    if (!m_devicePickerView) {
        return;
    }
    auto flyoutState = m_pickerFlyoutState.load();
    if (flyoutState != PickerFlyoutState::Open) {
        return;
    }
    if (!m_pickerFlyout || !m_pickerFlyout.Content()) {
        return;
    }
    try {
        auto impl = m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
        impl->RefreshDeviceStates();
        ReconcileTrayStateFromDeviceSnapshot(L"flyout-refresh-device-states");
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to refresh picker device state", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[TrayController] ERROR: failed to refresh picker device state", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[TrayController] ERROR: failed to refresh picker device state");
    }
}

void TrayController::OnThemeChanged() {
    if (m_trayIcon) {
        m_trayIcon->UpdateTheme();
    }
}

void TrayController::AdvanceConnectingFrame() {
    if (m_trayIcon) {
        m_trayIcon->AdvanceConnectingFrame();
    }
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

void TrayController::HandleTrayMessage([[maybe_unused]] WPARAM wParam, LPARAM lParam) {
    if (m_isTearingDown.load()) return;

    auto dispatcher = m_mainWindow ? m_mainWindow.DispatcherQueue() : nullptr;
    if (dispatcher && !dispatcher.HasThreadAccess()) {
        auto weak = weak_from_this();
        bool enqueued = dispatcher.TryEnqueue([weak, wParam, lParam]() {
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
                ShowDevicePicker();
            }
            break;

        case WM_LBUTTONDBLCLK:
            m_lastLeftDoubleClickTick = GetTickCount64();
            if (m_pickerFlyout && m_pickerFlyoutState.load() != PickerFlyoutState::Closed) {
                m_pickerFlyoutState.store(PickerFlyoutState::Closing);
                m_pickerFlyout.Hide();
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
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Internal Helpers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    impl->Initialize(
        m_deviceManager,
        [weak]() {
            auto self = weak.lock();
            if (self && !self->m_isTearingDown.load() && self->m_pickerFlyout) self->m_pickerFlyout.Hide();
        },
        [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User selected device: {0}", std::wstring(id));
            if (self->m_connectCallback) self->m_connectCallback(id);
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide();
        },
        [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User disconnected device: {0}", std::wstring(id));
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide();
            if (self->m_disconnectCallback) self->m_disconnectCallback(id);
        },
        [weak](winrt::hstring id) {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            DebugTrace(L"[TrayController] User reconnected device: {0}", std::wstring(id));
            if (self->m_pickerFlyout) self->m_pickerFlyout.Hide();
            auto dispatcher = self->m_mainWindow ? self->m_mainWindow.DispatcherQueue() : nullptr;
            if (dispatcher) {
                auto weakSelf = weak;
                bool queued = dispatcher.TryEnqueue([weakSelf, id]() {
                    if (auto queuedSelf = weakSelf.lock();
                        queuedSelf && !queuedSelf->m_isTearingDown.load() && queuedSelf->m_reconnectCallback) {
                        queuedSelf->m_reconnectCallback(id);
                    }
                });
                if (queued) {
                    return;
                }
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

Controls::Flyout TrayController::CreatePickerFlyout() {
    Controls::Flyout flyout;
    flyout.ShouldConstrainToRootBounds(false);
    flyout.Content(m_devicePickerView);

    auto weak = weak_from_this();
    flyout.Opened([weak](auto&, auto&) {
        auto self = weak.lock();
        if (self && !self->m_isTearingDown.load() && self->m_pickerFlyout) {
            self->m_pickerFlyoutState.store(PickerFlyoutState::Open);
            apc::ui::StripFlyoutPresenterStyle(
                self->m_pickerFlyout.Content().as<winrt::Microsoft::UI::Xaml::DependencyObject>());
        }
    });

    flyout.Closing([weak](auto&, auto&) {
        try {
            auto self = weak.lock();
            if (!self || self->m_isTearingDown.load()) return;
            self->m_pickerFlyoutState.store(PickerFlyoutState::Closing);
            if (self->m_devicePickerView) {
                auto impl =
                    self->m_devicePickerView.as<winrt::AudioPlaybackConnector2::implementation::DevicePickerView>();
                impl->CancelLoadDevices();
            }
            if (self->m_pickerFlyout) {
                self->m_pickerFlyout.Content(nullptr);
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closing handler failed", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[TrayController] ERROR: Picker flyout closing handler failed", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[TrayController] ERROR: Picker flyout closing handler failed");
        }
    });

    flyout.Closed([weak](auto&, auto&) {
        auto self = weak.lock();
        if (!self || self->m_isTearingDown.load()) return;
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
    });

    return flyout;
}

void TrayController::ReconcileTrayStateFromDeviceSnapshot(std::wstring_view reason) {
    if (!m_trayIcon || !m_deviceManager || m_isTearingDown.load()) {
        DebugTrace(L"[TrayController] Tray state reconcile skipped reason={0} hasTrayIcon={1} hasDeviceManager={2} "
                   L"tearingDown={3}",
                   reason,
                   m_trayIcon != nullptr,
                   m_deviceManager != nullptr,
                   m_isTearingDown.load());
        return;
    }

    auto connected = m_deviceManager->GetConnectedDevices();
    const bool hasConnections = !connected.empty();
    const bool hasBusyOperations = m_deviceManager->HasBusyOperations();

    TrayIconState desiredState = TrayIconState::Idle;
    if (hasConnections) {
        desiredState = TrayIconState::Connected;
    } else if (hasBusyOperations) {
        desiredState = TrayIconState::Connecting;
    }

    const auto currentState = m_trayIcon->State();
    const bool mismatch = currentState != desiredState;
    DebugTrace(L"[TrayController] Tray state reconcile reason={0} current={1} desired={2} mismatch={3} "
               L"connectedCount={4} hasBusyOperations={5}",
               reason,
               TrayIconStateToString(currentState),
               TrayIconStateToString(desiredState),
               mismatch,
               connected.size(),
               hasBusyOperations);

    if (mismatch) {
        DebugTrace(L"[TrayController] TRAY_STATE_MISMATCH reason={0} current={1} desired={2} connectedCount={3} "
                   L"hasBusyOperations={4}",
                   reason,
                   TrayIconStateToString(currentState),
                   TrayIconStateToString(desiredState),
                   connected.size(),
                   hasBusyOperations);
    }

    m_trayIcon->SetState(desiredState);
    UpdateTooltipFromConnections();
}

util::SettingsWindowPlacement TrayController::CalculateSettingsWindowPlacement() const {
    if (!m_trayIcon) return util::CalculateSettingsWindowPlacement();
    auto rect = m_trayIcon->GetIconRect();
    if (!rect) return util::CalculateSettingsWindowPlacement();

    return util::CalculateSettingsWindowPlacement(*rect);
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
