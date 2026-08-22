#include <pch.h>

#include <app/StartupUpdateCoordinator.hpp>

#include <core/SettingsStore.hpp>
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

winrt::Windows::Foundation::IAsyncAction
StartupUpdateCoordinator::CheckForUpdatesAsync(SettingsStore& settings,
                                               std::shared_ptr<NotificationService> notificationService,
                                               std::shared_ptr<UpdateCoordinator> updateCoordinator,
                                               std::atomic<bool>& exiting) {
    if (exiting.load() || !notificationService || !updateCoordinator) co_return;

    const auto now = UnixNowSeconds();
    const auto settingsSnapshot = settings.Snapshot();
    const auto shouldCheck =
        settingsSnapshot.Data.LastUpdateCheckUnixSeconds <= 0 ||
        settingsSnapshot.Data.LastUpdateCheckUnixSeconds > now ||
        now - settingsSnapshot.Data.LastUpdateCheckUnixSeconds >= c_startupUpdateCheckInterval.count();
    if (!shouldCheck || exiting.load()) co_return;

    // G04 remains unapproved, so automatic startup checks retain their existing delay and behavior.
    if (!co_await updateCoordinator->WaitForAutomaticCheckWindowAsync(c_automaticCheckStableDelay)) co_return;
    if (exiting.load()) co_return;

    winrt::apartment_context ui;
    const auto result = co_await updateCoordinator->CheckForUpdatesAsync(UpdateCheckReason::Automatic);
    co_await ui;
    if (exiting.load() || !notificationService || result.Status == UpdateCheckStatus::Failed ||
        result.Status == UpdateCheckStatus::Cancelled) {
        co_return;
    }

    const auto notificationSnapshot = settings.Snapshot();
    const auto shouldNotify = result.Status == UpdateCheckStatus::UpdateAvailable && !result.LatestVersion.empty() &&
                              notificationSnapshot.Data.LastNotifiedUpdateVersion != result.LatestVersion;
    auto notifiedVersion = std::optional<std::wstring>{};
    if (shouldNotify && !exiting.load() && notificationService->ShowUpdateAvailable(result.LatestVersion)) {
        notifiedVersion = result.LatestVersion;
    }
    static_cast<void>(settings.RecordUpdateCheckMetadata(now, std::move(notifiedVersion)));
}
