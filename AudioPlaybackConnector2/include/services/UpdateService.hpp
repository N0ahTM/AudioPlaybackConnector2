#pragma once

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Update Service ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class UpdateCheckStatus { UpToDate, UpdateAvailable, Failed };

struct UpdateCheckResult {
    UpdateCheckStatus Status = UpdateCheckStatus::Failed;
    std::wstring CurrentVersion;
    std::wstring LatestVersion;
    std::wstring ReleaseUrl;
    std::wstring AppInstallerUrl;
    std::wstring ErrorMessage;
};

class UpdateService {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static UpdateCheckResult CheckForUpdates();
    static std::wstring CurrentVersionString();
    static std::wstring_view AppInstallerUrl();
    static std::wstring_view LatestReleaseApiUrl();
    static std::wstring_view LatestReleasePageUrl();
    static winrt::fire_and_forget LaunchAppInstallerAsync();
    static winrt::fire_and_forget LaunchReleasePageAsync(std::wstring releaseUrl);
};
