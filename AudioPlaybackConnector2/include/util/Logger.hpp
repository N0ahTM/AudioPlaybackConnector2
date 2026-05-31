#pragma once

#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger ////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

namespace details {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Environment Helpers ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring GetEnvironmentVariableValue(wchar_t const* name) {
    DWORD const required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};

    std::wstring value(required, L'\0');
    DWORD const length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required) return {};

    value.resize(length);
    return value;
}

inline std::wstring GetTempDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
    if (length == 0) return {};

    if (length >= path.size()) {
        path.assign(static_cast<size_t>(length) + 1, L'\0');
        length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
        if (length == 0 || length >= path.size()) return {};
    }

    path.resize(length);
    return path;
}

inline bool EnsureDirectory(std::filesystem::path const& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

inline std::optional<std::wstring> TryGetCurrentPackageFullName() {
    UINT32 length = 0;
    LONG status = ::GetCurrentPackageFullName(&length, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER || length <= 1) {
        return std::nullopt;
    }

    std::wstring fullName(length, L'\0');
    status = ::GetCurrentPackageFullName(&length, fullName.data());
    if (status != ERROR_SUCCESS || length <= 1) {
        return std::nullopt;
    }

    fullName.resize(length - 1);
    return fullName;
}

inline std::filesystem::path ResolvePackageLocalDirectory(std::wstring_view localAppDataRoot) {
    if (localAppDataRoot.empty()) return {};
    auto packageFullName = TryGetCurrentPackageFullName();
    if (!packageFullName || packageFullName->empty()) return {};
    return std::filesystem::path(localAppDataRoot) / L"Packages" / *packageFullName / L"LocalCache" / L"Local" /
           L"AudioPlaybackConnector2";
}

inline std::filesystem::path ResolveLogPath() noexcept {
    try {
        auto localAppData = GetEnvironmentVariableValue(L"LOCALAPPDATA");
        if (!localAppData.empty()) {
            auto packageDir = ResolvePackageLocalDirectory(localAppData);
            if (!packageDir.empty() && EnsureDirectory(packageDir)) {
                return packageDir / L"AudioPlaybackConnector2.log";
            }

            auto defaultDir = std::filesystem::path(localAppData) / L"AudioPlaybackConnector2";
            if (EnsureDirectory(defaultDir)) {
                return defaultDir / L"AudioPlaybackConnector2.log";
            }
        }

        auto tempDirectory = GetTempDirectory();
        if (tempDirectory.empty()) return {};
        auto tempLogDir = std::filesystem::path(tempDirectory) / L"AudioPlaybackConnector2";
        if (!EnsureDirectory(tempLogDir)) return {};
        return tempLogDir / L"AudioPlaybackConnector2.log";
    } catch (...) {
        return {};
    }
}

} // namespace details

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Log Path //////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::filesystem::path const& GetCachedLogPath() {
    static std::filesystem::path const s_path = details::ResolveLogPath();
    return s_path;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger Class //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class Logger {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Singleton /////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static Logger& Instance() noexcept {
        static Logger s_instance;
        return s_instance;
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Lifetime //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    Logger(Logger const&) = delete;
    Logger& operator=(Logger const&) = delete;

    ~Logger() noexcept { Shutdown(); }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Logging ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Enqueue(std::string message) noexcept {
        try {
            {
                std::lock_guard lock(m_queueMutex);
                if (m_shutdown || !m_thread.joinable()) return;
                m_queue.push(std::move(message));
            }
            m_cv.notify_one();
        } catch (...) {
        }
    }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Construction //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    Logger() noexcept {
        try {
            m_path = GetCachedLogPath();
            if (!m_path.empty()) {
                m_thread = std::thread([this] { Run(); });
            }
        } catch (...) {
        }
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Control ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Shutdown() noexcept {
        {
            std::lock_guard lock(m_queueMutex);
            m_shutdown = true;
        }
        m_cv.notify_one();
        try {
            if (m_thread.joinable()) m_thread.join();
        } catch (...) {
        }
    }

    void Disable() noexcept {
        try {
            std::lock_guard lock(m_queueMutex);
            m_shutdown = true;
            std::queue<std::string> empty;
            m_queue.swap(empty);
        } catch (...) {
        }
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Worker Thread /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Run() noexcept {
        try {
            RunWorker();
        } catch (...) {
            Disable();
        }
    }

    void RunWorker() {
        wil::unique_hfile file;
        ULONGLONG bytesWritten = 0;
        constexpr ULONGLONG c_maxLogBytes = 2ull * 1024ull * 1024ull;

        auto tryOpen = [&]() {
            file.reset(CreateFileW(m_path.c_str(),
                                   FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
            if (!file) return;
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(file.get(), &sz)) bytesWritten = static_cast<ULONGLONG>(sz.QuadPart);
        };

        auto rotate = [&]() {
            file.reset();
            auto backup = m_path;
            backup += L".1";
            DeleteFileW(backup.c_str());
            MoveFileExW(m_path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
            bytesWritten = 0;
            tryOpen();
        };

        auto writeBatch = [&](std::queue<std::string>& batch) {
            while (!batch.empty()) {
                if (!file) tryOpen();
                if (bytesWritten >= c_maxLogBytes) rotate();
                if (!file) return;

                auto const& msg = batch.front();
                DWORD written = 0;
                if (WriteFile(file.get(), msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr))
                    bytesWritten += written;

                batch.pop();
            }
        };

        tryOpen();

        while (true) {
            std::queue<std::string> batch;
            bool shouldExit = false;
            {
                std::unique_lock lock(m_queueMutex);
                m_cv.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });
                batch.swap(m_queue);
                shouldExit = m_shutdown;
            }
            writeBatch(batch);
            if (shouldExit) break;
        }
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::filesystem::path m_path;
    std::thread m_thread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<std::string> m_queue;
    bool m_shutdown = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Write Log Line ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline void WriteLogLine(std::wstring_view message) noexcept {
    try {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        auto line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [T:{}] {}\r\n",
                                st.wYear,
                                st.wMonth,
                                st.wDay,
                                st.wHour,
                                st.wMinute,
                                st.wSecond,
                                st.wMilliseconds,
                                GetCurrentThreadId(),
                                message);
        Logger::Instance().Enqueue(Utf16ToUtf8(line));
    } catch (...) {
    }
}

} // namespace util

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Debug Logging /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------------------------------------*/
/*//////// String DebugTrace /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline void DebugTrace(std::wstring_view message) noexcept {
    util::WriteLogLine(message);
#ifdef _DEBUG
    try {
        std::wstring output(message);
        output.push_back(L'\n');
        OutputDebugStringW(output.c_str());
    } catch (...) {
    }
#endif
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Formatted DebugTrace //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

template <typename... Args> inline void DebugTrace(std::wstring_view fmt, Args&&... args) noexcept {
    try {
        DebugTrace(std::vformat(fmt, std::make_wformat_args(args...)));
    } catch (...) {
        DebugTrace(fmt);
    }
}
