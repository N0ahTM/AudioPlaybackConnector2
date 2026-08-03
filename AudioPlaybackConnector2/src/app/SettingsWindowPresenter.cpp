#include <pch.h>

#include <app/SettingsWindowPresenter.hpp>

#include <SettingsWindow/SettingsWindow.xaml.h>
#include <services/TrayController.hpp>
#include <services/UpdateCoordinator.hpp>
#include <ui/XamlWindowInterop.hpp>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsWindowPresenter::~SettingsWindowPresenter() {
    Close();
}

bool SettingsWindowPresenter::Show(std::shared_ptr<ISettingsController> settingsController,
                                   std::shared_ptr<TrayController> trayController,
                                   std::shared_ptr<UpdateCoordinator> updateCoordinator,
                                   std::function<void()> saveSettings) {
    DebugTrace(L"[SettingsWindowPresenter] Show()");
    if (m_settingsWindow) {
        auto hwnd = util::GetWindowHandle(m_settingsWindow);
        if (hwnd) {
            ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
            DebugTrace(L"[SettingsWindowPresenter] SettingsWindow brought to foreground");
            return true;
        }
        return false;
    }

    try {
        m_settingsWindow = winrt::AudioPlaybackConnector2::SettingsWindow();
        auto defaultPlacement =
            trayController ? trayController->GetSettingsWindowPlacement() : util::CalculateSettingsWindowPlacement();
        auto placement = defaultPlacement;

        if (settingsController) {
            auto snapshot = settingsController->Snapshot();
            if (snapshot.SettingsWindowBounds) {
                placement = util::CalculateSettingsWindowPlacementFromBounds(
                    POINT{snapshot.SettingsWindowBounds->X, snapshot.SettingsWindowBounds->Y},
                    SIZE{snapshot.SettingsWindowBounds->Width, snapshot.SettingsWindowBounds->Height},
                    snapshot.SettingsWindowBounds->Dpi);
            }
        }

        auto impl = m_settingsWindow.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        if (impl) {
            impl->SetSettingsController(std::move(settingsController));
            impl->SetUpdateCoordinator(std::move(updateCoordinator));
            impl->SetDefaultPlacement(defaultPlacement);
            impl->SetTargetPlacement(placement);
        }

        // Move off-screen at the final size before Activate() so XAML never measures against a tiny 1x1 window.
        auto appWindow = m_settingsWindow.AppWindow();
        if (appWindow) {
            appWindow.Move({-32000, -32000});
            appWindow.Resize({placement.size.cx, placement.size.cy});
        }

        m_settingsWindow.Activate();

        m_saveSettingsOnClose = std::move(saveSettings);
        m_settingsWindowClosedToken = m_settingsWindow.Closed([this](auto&, auto&) { HandleWindowClosed(); });
        m_settingsWindowClosedTokenRegistered = true;
        DebugTrace(L"[SettingsWindowPresenter] SettingsWindow created off-screen (hidden until ready)");
        return true;
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow: 0x{0:08X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
        m_settingsWindow = nullptr;
        return false;
    } catch (std::exception const& ex) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow: {0}",
                   util::Utf8ToUtf16(ex.what()));
        m_settingsWindow = nullptr;
        return false;
    } catch (...) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow");
        m_settingsWindow = nullptr;
        return false;
    }
}

void SettingsWindowPresenter::Close() noexcept {
    if (!m_settingsWindow) return;

    auto window = m_settingsWindow;
    try {
        window.Close();
    } catch (...) {
    }
    if (m_settingsWindow) {
        HandleWindowClosed();
    }
}

void SettingsWindowPresenter::HandleWindowClosed() noexcept {
    DebugTrace(L"[SettingsWindowPresenter] SettingsWindow closed");
    RevokeWindowClosedHandler();
    m_settingsWindow = nullptr;
    if (m_saveSettingsOnClose) {
        try {
            m_saveSettingsOnClose();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close");
        }
    }
    m_saveSettingsOnClose = nullptr;
}

void SettingsWindowPresenter::RevokeWindowClosedHandler() noexcept {
    if (!m_settingsWindow || !m_settingsWindowClosedTokenRegistered) return;

    try {
        m_settingsWindow.Closed(m_settingsWindowClosedToken);
    } catch (...) {
    }
    m_settingsWindowClosedToken = {};
    m_settingsWindowClosedTokenRegistered = false;
}
