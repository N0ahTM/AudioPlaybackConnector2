#pragma once

#include <atomic>

class NotificationService;
class SettingsStore;
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
    CheckForUpdatesAsync(SettingsStore& settings,
                         std::shared_ptr<NotificationService> notificationService,
                         std::shared_ptr<UpdateCoordinator> updateCoordinator,
                         std::atomic<bool>& exiting);
};
