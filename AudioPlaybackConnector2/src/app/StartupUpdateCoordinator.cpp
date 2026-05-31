#include <pch.h>

#include <app/StartupUpdateCoordinator.hpp>

#include <core/Settings.hpp>
#include <services/NotificationService.hpp>
#include <services/UpdateService.hpp>

namespace {
constexpr std::chrono::seconds c_startupUpdateCheckInterval{std::chrono::hours{24}};

int64_t UnixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::Windows::Foundation::IAsyncAction StartupUpdateCoordinator::CheckForUpdatesAsync(
    Settings& settings, std::shared_ptr<NotificationService> notificationService, std::atomic<bool>& exiting) {
    if (exiting.load() || !notificationService) co_return;

    const auto now = UnixNowSeconds();
    bool shouldCheck = false;
    {
        auto locked = settings.LockExclusiveData();
        shouldCheck = locked->LastUpdateCheckUnixSeconds <= 0 ||
                      now - locked->LastUpdateCheckUnixSeconds >= c_startupUpdateCheckInterval.count();
        if (shouldCheck) {
            locked->LastUpdateCheckUnixSeconds = now;
        }
    }

    if (!shouldCheck) co_return;
    if (exiting.load()) co_return;
    settings.Save(GetModuleHandleW(nullptr));

    winrt::apartment_context ui;
    auto result = co_await UpdateService::CheckForUpdatesAsync();
    co_await ui;

    if (exiting.load() || !notificationService) co_return;
    if (result.Status != UpdateCheckStatus::UpdateAvailable || result.LatestVersion.empty()) co_return;

    bool shouldNotify = false;
    {
        auto locked = settings.LockExclusiveData();
        if (locked->LastNotifiedUpdateVersion != result.LatestVersion) {
            locked->LastNotifiedUpdateVersion = result.LatestVersion;
            shouldNotify = true;
        }
    }

    if (!shouldNotify) co_return;
    if (exiting.load()) co_return;
    settings.Save(GetModuleHandleW(nullptr));
    notificationService->ShowUpdateAvailable(result.LatestVersion);
}
