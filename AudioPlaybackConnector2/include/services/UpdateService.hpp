#pragma once

#include <coroutine>
#include <exception>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <winrt/base.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Update Service ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class UpdateCheckStatus { UpToDate, UpdateAvailable, Failed, Cancelled };

struct UpdateCheckResult {
    UpdateCheckStatus Status = UpdateCheckStatus::Failed;
    std::wstring CurrentVersion;
    std::wstring LatestVersion;
    std::wstring ReleaseUrl;
    std::wstring AppInstallerUrl;
    std::wstring ErrorMessage;
};

class [[nodiscard("Update checks are lazy and must be awaited")]] UpdateCheckTask {
public:
    struct promise_type {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
                auto continuation = handle.promise().Continuation;
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        UpdateCheckTask get_return_object() noexcept;
        std::suspend_always initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }
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
    Handle await_suspend(std::coroutine_handle<> continuation) noexcept {
        m_handle.promise().Continuation = continuation;
        return m_handle;
    }
    UpdateCheckResult await_resume() {
        if (m_handle.promise().Exception) {
            std::rethrow_exception(m_handle.promise().Exception);
        }
        return std::move(m_handle.promise().Result);
    }

private:
    Handle m_handle;
};

inline UpdateCheckTask UpdateCheckTask::promise_type::get_return_object() noexcept {
    return UpdateCheckTask{UpdateCheckTask::Handle::from_promise(*this)};
}

class UpdateService {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static UpdateCheckTask CheckForUpdatesAsync(std::stop_token stopToken = {});
    static std::wstring CurrentVersionString();
    static std::wstring_view AppInstallerUrl();
    static std::wstring_view LatestReleaseApiUrl();
    static std::wstring_view LatestReleasePageUrl();
    static winrt::fire_and_forget LaunchAppInstallerAsync();
    static winrt::fire_and_forget LaunchReleasePageAsync(std::wstring releaseUrl);
};
