#include <pch.h>

#include <app/SettingsWindowPresenter.hpp>

#include <SettingsWindow/SettingsWindow.xaml.h>
#include <services/TrayController.hpp>
#include <services/UpdateCoordinator.hpp>
#include <ui/XamlWindowInterop.hpp>
#include <util/Util.hpp>

struct SettingsWindowPresenter::WindowState {
    winrt::Microsoft::UI::Xaml::Window Window{nullptr};
    winrt::event_token ClosedToken{};
    std::function<void()> SaveSettings;
    bool ClosedTokenRegistered = false;
    bool Closing = false;
    bool Closed = false;
    bool Activated = false;
};

struct SettingsWindowPresenter::PresenterState {
    std::shared_ptr<WindowState> Current;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsWindowPresenter::SettingsWindowPresenter() : m_state(std::make_shared<PresenterState>()) {}

SettingsWindowPresenter::~SettingsWindowPresenter() {
    auto owner = std::exchange(m_state, nullptr);
    if (!owner || !owner->Current) return;

    auto state = owner->Current;
    if (!CloseWindow(owner, state, false)) {
        AbandonWindow(owner, state);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SettingsWindowPresenter::Show(std::shared_ptr<ISettingsController> settingsController,
                                   std::shared_ptr<TrayController> trayController,
                                   std::shared_ptr<UpdateCoordinator> updateCoordinator,
                                   std::function<void()> saveSettings) {
    DebugTrace(L"[SettingsWindowPresenter] Show()");
    auto owner = m_state;
    if (!owner) return false;

    if (auto current = owner->Current) {
        if (current->Closed || !current->Window) {
            owner->Current.reset();
        } else {
            try {
                auto hwnd = util::GetWindowHandle(current->Window);
                if (current->Activated && hwnd && IsWindow(hwnd)) {
                    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
                    SetForegroundWindow(hwnd);
                    DebugTrace(L"[SettingsWindowPresenter] SettingsWindow brought to foreground");
                    return true;
                }
            } catch (winrt::hresult_error const& ex) {
                util::DebugTraceException(L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow", ex);
            } catch (std::exception const& ex) {
                util::DebugTraceException(L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow", ex);
            } catch (...) {
                util::DebugTraceUnknownException(
                    L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow");
            }

            DebugTrace(L"[SettingsWindowPresenter] Closing stale SettingsWindow before replacement");
            if (!CloseWindow(owner, current, true)) {
                DebugTrace(L"[SettingsWindowPresenter] ERROR: stale SettingsWindow could not be closed");
                return false;
            }
        }
    }

    std::shared_ptr<WindowState> candidate;
    try {
        candidate = std::make_shared<WindowState>();
        candidate->Window = winrt::AudioPlaybackConnector2::SettingsWindow();
        candidate->SaveSettings = std::move(saveSettings);
        owner->Current = candidate;

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

        auto impl = candidate->Window.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        impl->SetSettingsController(std::move(settingsController));
        impl->SetUpdateCoordinator(std::move(updateCoordinator));
        impl->SetDefaultPlacement(defaultPlacement);
        impl->SetTargetPlacement(placement);

        auto appWindow = candidate->Window.AppWindow();
        if (appWindow) {
            appWindow.Move({-32000, -32000});
            appWindow.Resize({placement.size.cx, placement.size.cy});
        }

        auto weakOwner = std::weak_ptr<PresenterState>(owner);
        auto weakCandidate = std::weak_ptr<WindowState>(candidate);
        candidate->ClosedToken =
            candidate->Window.Closed([weakOwner, weakCandidate](auto const& sender, auto&) noexcept {
                try {
                    auto state = weakCandidate.lock();
                    if (!state) return;
                    auto closedWindow = sender.template try_as<winrt::Microsoft::UI::Xaml::Window>();
                    if (!closedWindow) return;
                    HandleWindowClosed(weakOwner.lock(), state, closedWindow);
                } catch (winrt::hresult_error const& ex) {
                    util::DebugTraceException(L"[SettingsWindowPresenter] Closed callback failed", ex);
                } catch (std::exception const& ex) {
                    util::DebugTraceException(L"[SettingsWindowPresenter] Closed callback failed", ex);
                } catch (...) {
                    util::DebugTraceUnknownException(L"[SettingsWindowPresenter] Closed callback failed");
                }
            });
        candidate->ClosedTokenRegistered = true;
        candidate->Window.Activate();
        if (candidate->Closed || owner->Current != candidate) {
            DebugTrace(L"[SettingsWindowPresenter] SettingsWindow closed while activating");
            return false;
        }
        candidate->Activated = true;

        DebugTrace(L"[SettingsWindowPresenter] SettingsWindow created off-screen (hidden until ready)");
        return true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] Failed to create SettingsWindow", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] Failed to create SettingsWindow", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindowPresenter] Failed to create SettingsWindow");
    }

    if (candidate && !CloseWindow(owner, candidate, false)) {
        DebugTrace(L"[SettingsWindowPresenter] ERROR: failed SettingsWindow rollback could not close its window");
    }
    return false;
}

bool SettingsWindowPresenter::Close(bool saveOnClose) noexcept {
    auto owner = m_state;
    if (!owner || !owner->Current) return true;
    return CloseWindow(owner, owner->Current, saveOnClose);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SettingsWindowPresenter::CloseWindow(std::shared_ptr<PresenterState> const& owner,
                                          std::shared_ptr<WindowState> const& state,
                                          bool saveOnClose) noexcept {
    if (!state || state->Closed || !state->Window) {
        if (owner && owner->Current == state) owner->Current.reset();
        return true;
    }
    if (state->Closing) return false;

    if (!saveOnClose) state->SaveSettings = nullptr;
    auto window = state->Window;
    state->Closing = true;
    try {
        window.Close();
    } catch (winrt::hresult_error const& ex) {
        state->Closing = false;
        util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow", ex);
        return state->Closed;
    } catch (std::exception const& ex) {
        state->Closing = false;
        util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow", ex);
        return state->Closed;
    } catch (...) {
        state->Closing = false;
        util::DebugTraceUnknownException(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow");
        return state->Closed;
    }

    if (!state->Closed) HandleWindowClosed(owner, state, window);
    return state->Closed;
}

void SettingsWindowPresenter::HandleWindowClosed(std::shared_ptr<PresenterState> const& owner,
                                                 std::shared_ptr<WindowState> const& state,
                                                 winrt::Microsoft::UI::Xaml::Window const& closedWindow) noexcept {
    if (!state || state->Closed || !state->Window || state->Window != closedWindow) return;

    DebugTrace(L"[SettingsWindowPresenter] SettingsWindow closed");
    state->Closed = true;
    state->Closing = false;
    RevokeWindowClosedHandler(state);
    state->Window = nullptr;
    auto saveSettings = std::exchange(state->SaveSettings, nullptr);
    if (owner && owner->Current == state) owner->Current.reset();

    if (!saveSettings) return;
    try {
        saveSettings();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindowPresenter] ERROR: failed to save settings on close");
    }
}

void SettingsWindowPresenter::RevokeWindowClosedHandler(std::shared_ptr<WindowState> const& state) noexcept {
    if (!state || !state->Window || !state->ClosedTokenRegistered) return;

    auto token = std::exchange(state->ClosedToken, {});
    state->ClosedTokenRegistered = false;
    try {
        state->Window.Closed(token);
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler");
    }
}

void SettingsWindowPresenter::AbandonWindow(std::shared_ptr<PresenterState> const& owner,
                                            std::shared_ptr<WindowState> const& state) noexcept {
    if (!state) return;
    state->SaveSettings = nullptr;
    state->Closed = true;
    state->Closing = false;
    RevokeWindowClosedHandler(state);
    state->Window = nullptr;
    if (owner && owner->Current == state) owner->Current.reset();
}
