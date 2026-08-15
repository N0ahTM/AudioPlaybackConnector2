#pragma once

#include <atomic>
#include <functional>

class NotificationService;
class Settings;
class UpdateCoordinator;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Startup Update Coordinator ////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class StartupUpdateCoordinator {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static winrt::Windows::Foundation::IAsyncAction
    CheckForUpdatesAsync(Settings& settings,
                         std::shared_ptr<NotificationService> notificationService,
                         std::shared_ptr<UpdateCoordinator> updateCoordinator,
                         std::function<void()> requestSettingsSave,
                         std::atomic<bool>& exiting);
};
