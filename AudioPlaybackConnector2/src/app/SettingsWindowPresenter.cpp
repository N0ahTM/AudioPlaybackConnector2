#include <pch.h>

#include <app/SettingsWindowPresenter.hpp>

#include <SettingsWindow/SettingsWindow.xaml.h>
#include <services/TrayController.hpp>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void SettingsWindowPresenter::Show(std::shared_ptr<ISettingsController> settingsController,
                                   std::shared_ptr<TrayController> trayController,
                                   std::function<void()> saveSettings) {
    DebugTrace(L"[SettingsWindowPresenter] Show()");
    if (m_settingsWindow) {
        auto hwnd = util::GetWindowHandle(m_settingsWindow);
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            DebugTrace(L"[SettingsWindowPresenter] SettingsWindow brought to foreground");
        }
        return;
    }

    try {
        m_settingsWindow = winrt::AudioPlaybackConnector2::SettingsWindow();
        auto placement =
            trayController ? trayController->GetSettingsWindowPlacement() : util::CalculateSettingsWindowPlacement();

        auto impl = m_settingsWindow.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        if (impl) {
            impl->SetSettingsController(std::move(settingsController));
            impl->SetTargetPlacement(placement);
        }

        // Move off-screen at the final size before Activate() so XAML never measures against a tiny 1x1 window.
        auto appWindow = m_settingsWindow.AppWindow();
        if (appWindow) {
            appWindow.Move({-32000, -32000});
            appWindow.Resize({placement.size.cx, placement.size.cy});
        }

        if (auto hwnd = util::GetWindowHandle(m_settingsWindow)) {
            util::ApplyNativeMicaBackdrop(hwnd);
        }

        m_settingsWindow.Activate();

        m_settingsWindow.Closed([this, saveSettings = std::move(saveSettings)](auto&, auto&) mutable {
            DebugTrace(L"[SettingsWindowPresenter] SettingsWindow closed");
            m_settingsWindow = nullptr;
            if (saveSettings) {
                saveSettings();
            }
        });
        DebugTrace(L"[SettingsWindowPresenter] SettingsWindow created off-screen (hidden until ready)");
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow: 0x{0:08X} {1}",
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
        m_settingsWindow = nullptr;
    } catch (std::exception const& ex) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow: {0}",
                   util::Utf8ToUtf16(ex.what()));
        m_settingsWindow = nullptr;
    } catch (...) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: Failed to create SettingsWindow");
        m_settingsWindow = nullptr;
    }
}

void SettingsWindowPresenter::Close() noexcept {
    if (!m_settingsWindow) return;

    try {
        m_settingsWindow.Close();
    } catch (...) {
    }
    m_settingsWindow = nullptr;
}
