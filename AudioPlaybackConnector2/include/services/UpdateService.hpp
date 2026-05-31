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

class UpdateCheckTask {
public:
    struct promise_type {
        UpdateCheckTask get_return_object() noexcept;
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept;
        void return_value(UpdateCheckResult value) noexcept { Result = std::move(value); }
        void unhandled_exception() noexcept { Exception = std::current_exception(); }

        UpdateCheckResult Result;
        std::exception_ptr Exception;
        std::coroutine_handle<> Continuation;
    };

    using Handle = std::coroutine_handle<promise_type>;

    explicit UpdateCheckTask(Handle handle) noexcept : m_handle(handle) {}
    UpdateCheckTask(UpdateCheckTask const&) = delete;
    UpdateCheckTask& operator=(UpdateCheckTask const&) = delete;
    UpdateCheckTask(UpdateCheckTask&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    UpdateCheckTask& operator=(UpdateCheckTask&& other) noexcept {
        if (this != &other) {
            if (m_handle) {
                m_handle.destroy();
            }
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    ~UpdateCheckTask() {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) noexcept;
    UpdateCheckResult await_resume();

private:
    Handle m_handle;
};

class UpdateService {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static UpdateCheckTask CheckForUpdatesAsync();
    static std::wstring CurrentVersionString();
    static std::wstring_view AppInstallerUrl();
    static std::wstring_view LatestReleaseApiUrl();
    static std::wstring_view LatestReleasePageUrl();
    static winrt::fire_and_forget LaunchAppInstallerAsync();
    static winrt::fire_and_forget LaunchReleasePageAsync(std::wstring releaseUrl);
};
