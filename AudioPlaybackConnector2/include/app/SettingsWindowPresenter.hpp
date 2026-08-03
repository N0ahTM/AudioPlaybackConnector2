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

    ~SettingsWindowPresenter();

    [[nodiscard]] bool Show(std::shared_ptr<ISettingsController> settingsController,
                            std::shared_ptr<TrayController> trayController,
                            std::shared_ptr<UpdateCoordinator> updateCoordinator,
                            std::function<void()> saveSettings);
    void Close() noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void HandleWindowClosed() noexcept;
    void RevokeWindowClosedHandler() noexcept;

    winrt::Microsoft::UI::Xaml::Window m_settingsWindow{nullptr};
    winrt::event_token m_settingsWindowClosedToken{};
    std::function<void()> m_saveSettingsOnClose;
    bool m_settingsWindowClosedTokenRegistered = false;
};
