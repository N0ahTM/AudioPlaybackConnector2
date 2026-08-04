#pragma once

#include <functional>
#include <memory>

class ISettingsController;
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
                            std::shared_ptr<TrayController> trayController,
                            std::shared_ptr<UpdateCoordinator> updateCoordinator,
                            std::function<void()> saveSettings);
    [[nodiscard]] bool Close(bool saveOnClose = true) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct PresenterState;
    struct WindowState;

    static bool CloseWindow(std::shared_ptr<PresenterState> const& owner,
                            std::shared_ptr<WindowState> const& state,
                            bool saveOnClose) noexcept;
    static void HandleWindowClosed(std::shared_ptr<PresenterState> const& owner,
                                   std::shared_ptr<WindowState> const& state,
                                   winrt::Microsoft::UI::Xaml::Window const& closedWindow) noexcept;
    static void RevokeWindowClosedHandler(std::shared_ptr<WindowState> const& state) noexcept;
    static void AbandonWindow(std::shared_ptr<PresenterState> const& owner,
                              std::shared_ptr<WindowState> const& state) noexcept;

    std::shared_ptr<PresenterState> m_state;
};
