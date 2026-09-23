#pragma once

#include <memory>
#include <string_view>

#include <util/Logger.hpp>
#include <ui/WindowPlacement.hpp>

class StringResources;

namespace winrt::Microsoft::UI::Xaml {
struct Window;
}

namespace apc::app {
class AppController;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings Window Presenter /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class SettingsWindowPresenter {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit SettingsWindowPresenter(util::LogSink log, std::shared_ptr<StringResources const> strings);
    ~SettingsWindowPresenter();

    SettingsWindowPresenter(SettingsWindowPresenter const&) = delete;
    SettingsWindowPresenter& operator=(SettingsWindowPresenter const&) = delete;

    [[nodiscard]] bool Show(std::shared_ptr<apc::app::AppController> appController,
                            util::SettingsWindowPlacement defaultPlacement);
    [[nodiscard]] bool Close() noexcept;
    [[nodiscard]] bool ShowHelp();
    void ApplyLanguage(std::wstring_view language);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct PresenterState;
    struct WindowState;

    static bool CloseWindow(std::shared_ptr<PresenterState> const& owner, std::shared_ptr<WindowState> state) noexcept;
    static void HandleWindowClosed(std::shared_ptr<PresenterState> const& owner,
                                   std::shared_ptr<WindowState> const& state,
                                   winrt::Microsoft::UI::Xaml::Window const& closedWindow) noexcept;
    static void RevokeWindowClosedHandler(std::shared_ptr<WindowState> const& state) noexcept;
    static void AbandonWindow(std::shared_ptr<PresenterState> const& owner,
                              std::shared_ptr<WindowState> const& state) noexcept;

    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    // UI-only owner; Close may synchronously invoke Closed and clear Current.
    // Operations retain state across that reentrancy; callbacks hold only weak references.
    std::shared_ptr<PresenterState> m_state;
};
