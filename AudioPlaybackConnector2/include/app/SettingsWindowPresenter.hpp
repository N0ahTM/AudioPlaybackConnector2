#pragma once

#include <core/StringResources.hpp>
#include <memory>

#include <util/Logger.hpp>
#include <ui/WindowPlacement.hpp>

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
    std::shared_ptr<PresenterState> m_state;
};
