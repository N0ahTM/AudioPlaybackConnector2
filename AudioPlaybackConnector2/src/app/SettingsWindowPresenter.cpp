#include <pch.h>

#include <app/SettingsWindowPresenter.hpp>
#include <app/AppController.hpp>
#include <core/StringResources.hpp>

#include <exception>
#include <optional>
#include <utility>

#include <SettingsWindow/SettingsWindow.xaml.h>
#include <ui/XamlWindowInterop.hpp>

struct SettingsWindowPresenter::WindowState {
    util::LogSink Log;
    winrt::Microsoft::UI::Xaml::Window Window{nullptr};
    winrt::event_token ClosedToken{};
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

SettingsWindowPresenter::SettingsWindowPresenter(util::LogSink log, std::shared_ptr<StringResources const> strings)
    : m_log(std::move(log)), m_strings(std::move(strings)), m_state(std::make_shared<PresenterState>()) {}

void SettingsWindowPresenter::ApplyLanguage(std::wstring_view language) {
    const auto state = m_state->Current;
    if (!state || state->Closed || state->Closing || !state->Window) return;
    state->Window.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>()->ApplyLanguage(language);
}

SettingsWindowPresenter::~SettingsWindowPresenter() {
    auto owner = std::exchange(m_state, nullptr);
    if (!owner || !owner->Current) return;

    auto state = owner->Current;
    if (!CloseWindow(owner, state)) {
        AbandonWindow(owner, state);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SettingsWindowPresenter::ShowHelp() {
    if (!m_state || !m_state->Current || m_state->Current->Closed || !m_state->Current->Window) return false;
    try {
        m_state->Current->Window.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>()->ShowHelpPage();
        return true;
    } catch (...) {
        m_log.UnknownException(L"[SettingsWindowPresenter] Failed to show help");
        return false;
    }
}

bool SettingsWindowPresenter::Show(std::shared_ptr<apc::app::AppController> appController,
                                   util::SettingsWindowPlacement defaultPlacement) {
    m_log.Trace(L"[SettingsWindowPresenter] Show()");
    auto owner = m_state;
    if (!owner) return false;

    if (auto current = owner->Current) {
        if (current->Closed || !current->Window) {
            owner->Current.reset();
        } else {
            try {
                auto hwnd = util::GetWindowHandle(current->Window);
                auto impl = current->Window.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
                if (current->Activated &&
                    impl->InitializationStatus() !=
                        winrt::AudioPlaybackConnector2::implementation::SettingsWindow::InitializationState::Failed &&
                    hwnd && IsWindow(hwnd)) {
                    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
                    SetForegroundWindow(hwnd);
                    m_log.Trace(L"[SettingsWindowPresenter] SettingsWindow brought to foreground");
                    return true;
                }
            } catch (winrt::hresult_error const& ex) {
                m_log.Exception(L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow", ex);
            } catch (std::exception const& ex) {
                m_log.Exception(L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow", ex);
            } catch (...) {
                m_log.UnknownException(L"[SettingsWindowPresenter] Failed to inspect existing SettingsWindow");
            }

            m_log.Trace(L"[SettingsWindowPresenter] Closing stale SettingsWindow before replacement");
            if (!CloseWindow(owner, current)) {
                m_log.Trace(L"[SettingsWindowPresenter] ERROR: stale SettingsWindow could not be closed");
                return false;
            }
        }
    }

    std::shared_ptr<WindowState> candidate;
    try {
        candidate = std::make_shared<WindowState>();
        candidate->Log = m_log;
        candidate->Window = winrt::AudioPlaybackConnector2::SettingsWindow();
        owner->Current = candidate;

        auto placement = defaultPlacement;
        std::optional<SettingsData> initialSettings;

        if (appController) {
            initialSettings = appController->Snapshot().Settings;
            if (initialSettings->SettingsWindowBounds) {
                placement = util::CalculateSettingsWindowPlacementFromBounds(
                    POINT{initialSettings->SettingsWindowBounds->X, initialSettings->SettingsWindowBounds->Y},
                    SIZE{initialSettings->SettingsWindowBounds->Width, initialSettings->SettingsWindowBounds->Height},
                    initialSettings->SettingsWindowBounds->Dpi);
            }
        }

        auto impl = candidate->Window.as<winrt::AudioPlaybackConnector2::implementation::SettingsWindow>();
        impl->SetAppController(std::move(appController), m_log, m_strings);
        if (initialSettings) impl->SetInitialSettingsSnapshot(std::move(*initialSettings));
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
            candidate->Window.Closed([weakOwner, weakCandidate, log = m_log](auto const& sender, auto&) noexcept {
                try {
                    auto state = weakCandidate.lock();
                    if (!state) return;
                    auto closedWindow = sender.template try_as<winrt::Microsoft::UI::Xaml::Window>();
                    if (!closedWindow) return;
                    HandleWindowClosed(weakOwner.lock(), state, closedWindow);
                } catch (winrt::hresult_error const& ex) {
                    log.Exception(L"[SettingsWindowPresenter] Closed callback failed", ex);
                } catch (std::exception const& ex) {
                    log.Exception(L"[SettingsWindowPresenter] Closed callback failed", ex);
                } catch (...) {
                    log.UnknownException(L"[SettingsWindowPresenter] Closed callback failed");
                }
            });
        candidate->ClosedTokenRegistered = true;
        candidate->Window.Activate();
        if (candidate->Closed || owner->Current != candidate) {
            m_log.Trace(L"[SettingsWindowPresenter] SettingsWindow closed while activating");
            return false;
        }
        if (impl->InitializationStatus() ==
            winrt::AudioPlaybackConnector2::implementation::SettingsWindow::InitializationState::Failed) {
            m_log.Trace(L"[SettingsWindowPresenter] SettingsWindow initialization did not complete");
            static_cast<void>(CloseWindow(owner, candidate));
            return false;
        }
        candidate->Activated = true;

        m_log.Trace(L"[SettingsWindowPresenter] SettingsWindow created off-screen (hidden until ready)");
        return true;
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[SettingsWindowPresenter] Failed to create SettingsWindow", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[SettingsWindowPresenter] Failed to create SettingsWindow", ex);
    } catch (...) {
        m_log.UnknownException(L"[SettingsWindowPresenter] Failed to create SettingsWindow");
    }

    if (candidate && !CloseWindow(owner, candidate)) {
        m_log.Trace(L"[SettingsWindowPresenter] ERROR: failed SettingsWindow rollback could not close its window");
    }
    return false;
}

bool SettingsWindowPresenter::Close() noexcept {
    auto owner = m_state;
    if (!owner || !owner->Current) return true;
    return CloseWindow(owner, owner->Current);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SettingsWindowPresenter::CloseWindow(std::shared_ptr<PresenterState> const& owner,
                                          std::shared_ptr<WindowState> state) noexcept {
    // Closed can synchronously reset owner->Current. Retain our own state while Close is on the stack.
    if (!state || state->Closed || !state->Window) {
        if (owner && owner->Current == state) owner->Current.reset();
        return true;
    }
    if (state->Closing) return false;

    auto window = state->Window;
    state->Closing = true;
    try {
        window.Close();
    } catch (winrt::hresult_error const& ex) {
        state->Closing = false;
        state->Log.Exception(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow", ex);
        return state->Closed;
    } catch (std::exception const& ex) {
        state->Closing = false;
        state->Log.Exception(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow", ex);
        return state->Closed;
    } catch (...) {
        state->Closing = false;
        state->Log.UnknownException(L"[SettingsWindowPresenter] ERROR: failed to close SettingsWindow");
        return state->Closed;
    }

    if (!state->Closed) HandleWindowClosed(owner, state, window);
    return state->Closed;
}

void SettingsWindowPresenter::HandleWindowClosed(std::shared_ptr<PresenterState> const& owner,
                                                 std::shared_ptr<WindowState> const& state,
                                                 winrt::Microsoft::UI::Xaml::Window const& closedWindow) noexcept {
    if (!state || state->Closed || !state->Window || state->Window != closedWindow) return;

    state->Log.Trace(L"[SettingsWindowPresenter] SettingsWindow closed");
    state->Closed = true;
    state->Closing = false;
    RevokeWindowClosedHandler(state);
    state->Window = nullptr;
    if (owner && owner->Current == state) owner->Current.reset();
}

void SettingsWindowPresenter::RevokeWindowClosedHandler(std::shared_ptr<WindowState> const& state) noexcept {
    if (!state || !state->Window || !state->ClosedTokenRegistered) return;

    auto token = std::exchange(state->ClosedToken, {});
    state->ClosedTokenRegistered = false;
    try {
        state->Window.Closed(token);
    } catch (winrt::hresult_error const& ex) {
        state->Log.Exception(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler", ex);
    } catch (std::exception const& ex) {
        state->Log.Exception(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler", ex);
    } catch (...) {
        state->Log.UnknownException(L"[SettingsWindowPresenter] Failed to revoke SettingsWindow Closed handler");
    }
}

void SettingsWindowPresenter::AbandonWindow(std::shared_ptr<PresenterState> const& owner,
                                            std::shared_ptr<WindowState> const& state) noexcept {
    if (!state) return;
    state->Closed = true;
    state->Closing = false;
    RevokeWindowClosedHandler(state);
    state->Window = nullptr;
    if (owner && owner->Current == state) owner->Current.reset();
}
