#include <pch.h>

#include <app/StartupUpdateCoordinator.hpp>

#include <core/Settings.hpp>
#include <services/NotificationService.hpp>
#include <services/UpdateCoordinator.hpp>

namespace {
constexpr std::chrono::seconds c_startupUpdateCheckInterval{std::chrono::hours{24}};
constexpr std::chrono::seconds c_automaticCheckStableDelay{30};

int64_t UnixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::Windows::Foundation::IAsyncAction
StartupUpdateCoordinator::CheckForUpdatesAsync(Settings& settings,
                                               std::shared_ptr<NotificationService> notificationService,
                                               std::shared_ptr<UpdateCoordinator> updateCoordinator,
                                               std::atomic<bool>& exiting) {
    if (exiting.load() || !notificationService || !updateCoordinator) co_return;

    const auto now = UnixNowSeconds();
    bool shouldCheck = false;
    {
        auto locked = settings.LockSharedData();
        shouldCheck = locked->LastUpdateCheckUnixSeconds <= 0 || locked->LastUpdateCheckUnixSeconds > now ||
                      now - locked->LastUpdateCheckUnixSeconds >= c_startupUpdateCheckInterval.count();
    }

    if (!shouldCheck) co_return;
    if (exiting.load()) co_return;

    if (!co_await updateCoordinator->WaitForAutomaticCheckWindowAsync(c_automaticCheckStableDelay)) co_return;
    if (exiting.load()) co_return;

    winrt::apartment_context ui;
    auto result = co_await updateCoordinator->CheckForUpdatesAsync(UpdateCheckReason::Automatic);
    co_await ui;

    if (exiting.load() || !notificationService) co_return;
    if (result.Status == UpdateCheckStatus::Failed || result.Status == UpdateCheckStatus::Cancelled) co_return;

    bool notificationShown = false;
    bool shouldNotify = false;
    {
        auto locked = settings.LockSharedData();
        shouldNotify = result.Status == UpdateCheckStatus::UpdateAvailable && !result.LatestVersion.empty() &&
                       locked->LastNotifiedUpdateVersion != result.LatestVersion;
    }

    if (shouldNotify) {
        if (exiting.load()) co_return;
        notificationShown = notificationService->ShowUpdateAvailable(result.LatestVersion);
    }

    {
        auto locked = settings.LockExclusiveData();
        locked.MarkChanged();
        locked->LastUpdateCheckUnixSeconds = now;
        if (notificationShown) {
            locked->LastNotifiedUpdateVersion = result.LatestVersion;
        }
    }
    settings.Save(GetModuleHandleW(nullptr));
}
