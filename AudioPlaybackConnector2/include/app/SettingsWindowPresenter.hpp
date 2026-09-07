#pragma once

#include <memory>

class ISettingsController;
class StartupTaskCoordinator;
class TrayController;
class UpdateCoordinator;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings Window Presenter /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class SettingsWindowPresenter {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    SettingsWindowPresenter();
    ~SettingsWindowPresenter();

    [[nodiscard]] bool Show(std::shared_ptr<ISettingsController> settingsController,
                            std::shared_ptr<StartupTaskCoordinator> startupTaskCoordinator,
                            std::shared_ptr<TrayController> trayController,
                            std::shared_ptr<UpdateCoordinator> updateCoordinator);
    [[nodiscard]] bool Close() noexcept;
    [[nodiscard]] bool ShowHelp();

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

    std::shared_ptr<PresenterState> m_state;
};
