#include <pch.h>
#include <util/Logger.hpp>

#include <cwctype>

namespace util {

namespace details {
constexpr std::size_t c_maxQueuedLogMessages = 10000;
constexpr std::size_t c_inMemoryLogTailLineCount = 100;

struct InMemoryLogTail {
    void Add(std::string line) noexcept {
        try {
            std::lock_guard lock(Mutex);
            Lines[NextIndex] = std::move(line);
            NextIndex = (NextIndex + 1) % Lines.size();
            if (Count < Lines.size()) ++Count;
        } catch (...) {
        }
    }

    bool TrySnapshot(std::vector<std::string>& snapshot) noexcept {
        try {
            if (!Mutex.try_lock()) return false;
            std::unique_lock lock(Mutex, std::adopt_lock);
            snapshot.clear();
            snapshot.reserve(Count);

            auto start = Count == Lines.size() ? NextIndex : 0;
            for (std::size_t i = 0; i < Count; ++i) {
                auto index = (start + i) % Lines.size();
                snapshot.push_back(Lines[index]);
            }
            return true;
        } catch (...) {
            snapshot.clear();
            return false;
        }
    }

    std::mutex Mutex;
    std::array<std::string, c_inMemoryLogTailLineCount> Lines;
    std::size_t NextIndex = 0;
    std::size_t Count = 0;
};

InMemoryLogTail& LogTail() noexcept {
    static InMemoryLogTail s_tail;
    return s_tail;
}

std::string FormatLogLine(std::wstring_view message) {
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
    return Utf16ToUtf8(line);
}

void AppendLogBytesDirect(std::string_view bytes) noexcept {
    try {
        if (bytes.empty()) return;
        auto const& path = GetCachedLogPath();
        if (path.empty()) return;

        wil::unique_hfile file(CreateFileW(path.c_str(),
                                           FILE_APPEND_DATA,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr,
                                           OPEN_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr));
        if (!file) return;

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto remaining = bytes.size() - offset;
            auto chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(file.get(), bytes.data() + offset, static_cast<DWORD>(chunk), &written, nullptr) ||
                written == 0) {
                return;
            }
            offset += written;
        }
        // Best-effort flush so crash tails survive immediate termination.
        (void)FlushFileBuffers(file.get());
    } catch (...) {
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Environment Helpers //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::wstring GetEnvironmentVariableValue(wchar_t const* name) {
    DWORD const required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};

    std::wstring value(required, L'\0');
    DWORD const length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required) return {};

    value.resize(length);
    return value;
}

std::wstring GetTempDirectory() {
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

bool EnsureDirectory(std::filesystem::path const& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

std::optional<std::wstring> TryGetCurrentPackageFamilyName() {
    UINT32 length = 0;
    LONG status = ::GetCurrentPackageFamilyName(&length, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER || length <= 1) {
        return std::nullopt;
    }

    std::wstring familyName(length, L'\0');
    status = ::GetCurrentPackageFamilyName(&length, familyName.data());
    if (status != ERROR_SUCCESS || length <= 1) {
        return std::nullopt;
    }

    familyName.resize(length - 1);
    return familyName;
}

std::filesystem::path ResolvePackageLocalDirectory(std::wstring_view localAppDataRoot) {
    if (localAppDataRoot.empty()) return {};
    auto packageFamilyName = TryGetCurrentPackageFamilyName();
    if (!packageFamilyName || packageFamilyName->empty()) return {};
    return std::filesystem::path(localAppDataRoot) / L"Packages" / *packageFamilyName / L"LocalCache" / L"Local" /
           L"AudioPlaybackConnector2";
}

std::filesystem::path ResolveLogPath() noexcept {
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

std::filesystem::path const& GetCachedLogPath() {
    static std::filesystem::path const s_path = details::ResolveLogPath();
    return s_path;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger Implementation //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct Logger::Impl {
    Impl() noexcept {
        try {
            Path = GetCachedLogPath();
            if (!Path.empty()) {
                Thread = std::thread([this] { Run(); });
            }
        } catch (...) {
        }
    }

    ~Impl() noexcept { Shutdown(); }

    void Enqueue(std::string message) noexcept {
        try {
            {
                std::lock_guard lock(QueueMutex);
                if (ShutdownRequested || !Thread.joinable()) return;

                if (Queue.size() >= details::c_maxQueuedLogMessages) {
                    auto const overflowCount = Queue.size() - details::c_maxQueuedLogMessages + 1;
                    for (std::size_t i = 0; i < overflowCount; ++i) {
                        Queue.pop();
                    }
                    DroppedMessages += overflowCount;
                }

                Queue.push(std::move(message));
            }
            Cv.notify_one();
        } catch (...) {
        }
    }

    void Shutdown() noexcept {
        {
            std::lock_guard lock(QueueMutex);
            ShutdownRequested = true;
        }
        Cv.notify_one();
        try {
            if (Thread.joinable()) Thread.join();
        } catch (...) {
        }
    }

    void Disable() noexcept {
        try {
            std::lock_guard lock(QueueMutex);
            ShutdownRequested = true;
            std::queue<std::string> empty;
            Queue.swap(empty);
        } catch (...) {
        }
        Cv.notify_one();
    }

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
            file.reset(CreateFileW(Path.c_str(),
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
            auto backup = Path;
            backup += L".1";
            DeleteFileW(backup.c_str());
            MoveFileExW(Path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
            bytesWritten = 0;
            tryOpen();
        };

        auto writeBatch = [&](std::queue<std::string>& batch) {
            while (!batch.empty()) {
                if (!file) tryOpen();
                if (bytesWritten >= c_maxLogBytes) rotate();
                if (!file) break;

                auto& msg = batch.front();
                if (msg.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
                    batch.pop();
                    continue;
                }

                std::size_t offset = 0;
                while (offset < msg.size()) {
                    DWORD written = 0;
                    if (!WriteFile(file.get(),
                                   msg.data() + offset,
                                   static_cast<DWORD>(msg.size() - offset),
                                   &written,
                                   nullptr) ||
                        written == 0) {
                        break;
                    }
                    offset += written;
                    bytesWritten += written;
                }
                if (offset != msg.size()) {
                    if (offset > 0) msg.erase(0, offset);
                    file.reset();
                    break;
                }
                batch.pop();
            }
        };

        auto makeDroppedNotice = [](std::size_t droppedCount) -> std::string {
            if (droppedCount == 0) return {};
            try {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                auto line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [T:{}] [Logger] Dropped {} queued "
                                        L"log message(s) due to backpressure\r\n",
                                        st.wYear,
                                        st.wMonth,
                                        st.wDay,
                                        st.wHour,
                                        st.wMinute,
                                        st.wSecond,
                                        st.wMilliseconds,
                                        GetCurrentThreadId(),
                                        droppedCount);
                return Utf16ToUtf8(line);
            } catch (...) {
                return {};
            }
        };

        tryOpen();
        auto retryDelay = std::chrono::milliseconds(100);
        constexpr auto c_maxRetryDelay = std::chrono::milliseconds(5000);

        while (true) {
            std::queue<std::string> batch;
            bool shouldExit = false;
            std::size_t droppedSinceLastFlush = 0;
            {
                std::unique_lock lock(QueueMutex);
                Cv.wait(lock, [this] { return !Queue.empty() || ShutdownRequested; });
                batch.swap(Queue);
                droppedSinceLastFlush = std::exchange(DroppedMessages, 0);
                shouldExit = ShutdownRequested;
            }
            if (droppedSinceLastFlush > 0) {
                auto notice = makeDroppedNotice(droppedSinceLastFlush);
                if (!notice.empty()) {
                    batch.push(std::move(notice));
                }
            }
            writeBatch(batch);
            if (!batch.empty() && !shouldExit) {
                // File temporarily unavailable; prepend leftovers back to the queue.
                std::unique_lock lock(QueueMutex);
                std::queue<std::string> newQueue;
                while (!batch.empty()) {
                    newQueue.push(std::move(batch.front()));
                    batch.pop();
                }
                while (!Queue.empty()) {
                    newQueue.push(std::move(Queue.front()));
                    Queue.pop();
                }
                Queue.swap(newQueue);

                Cv.wait_for(lock, retryDelay, [this] { return ShutdownRequested; });
                retryDelay = std::min(retryDelay * 2, c_maxRetryDelay);
            } else {
                retryDelay = std::chrono::milliseconds(100);
            }
            if (shouldExit) break;
        }
    }

    std::filesystem::path Path;
    std::thread Thread;
    std::mutex QueueMutex;
    std::condition_variable Cv;
    std::queue<std::string> Queue;
    std::size_t DroppedMessages = 0;
    bool ShutdownRequested = false;
};

Logger& Logger::Instance() noexcept {
    static Logger s_instance;
    return s_instance;
}

Logger::Logger() noexcept {
    try {
        m_impl = std::make_unique<Impl>();
    } catch (...) {
    }
}

Logger::~Logger() noexcept = default;

void Logger::Enqueue(std::string message) noexcept {
    if (m_impl) {
        m_impl->Enqueue(std::move(message));
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Write Log Line ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void WriteLogLine(std::wstring_view message) noexcept {
    try {
        auto line = details::FormatLogLine(message);
        details::LogTail().Add(line);
        Logger::Instance().Enqueue(std::move(line));
    } catch (...) {
    }
}

void FlushInMemoryLogTailToFile(std::wstring_view reason, std::uint32_t exceptionCode) noexcept {
    try {
        std::vector<std::string> lines;
        auto const snapshotAvailable = details::LogTail().TrySnapshot(lines);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        auto header = std::format(L"\r\n{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [T:{}] [Logger] BEGIN "
                                  L"in-memory diagnostic tail reason={} exception=0x{:08X} lines={}\r\n",
                                  st.wYear,
                                  st.wMonth,
                                  st.wDay,
                                  st.wHour,
                                  st.wMinute,
                                  st.wSecond,
                                  st.wMilliseconds,
                                  GetCurrentThreadId(),
                                  reason,
                                  exceptionCode,
                                  snapshotAvailable ? lines.size() : 0);

        std::string output = Utf16ToUtf8(header);
        if (!snapshotAvailable) {
            output += "[Logger] In-memory diagnostic tail unavailable because the buffer lock was busy\r\n";
        } else {
            for (auto const& line : lines) {
                output += line;
            }
        }
        output += "[Logger] END in-memory diagnostic tail\r\n\r\n";
        details::AppendLogBytesDirect(output);
    } catch (...) {
    }
}

} // namespace util

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Debug Logging /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

#ifndef _DEBUG
namespace {
bool ShouldPersistReleaseTrace(std::wstring_view message) noexcept {
    try {
        std::wstring lower;
        lower.reserve(message.size());
        for (wchar_t ch : message) {
            lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }

        constexpr std::array<std::wstring_view, 9> keywords{
            L"error", L"failed", L"warning", L"crash", L"exception", L"denied", L"timeout", L"corrupt", L"unsupported"};
        return std::ranges::any_of(keywords, [&](auto keyword) { return lower.find(keyword) != std::wstring::npos; });
    } catch (...) {
        return true;
    }
}
} // namespace
#endif

void DebugTrace(std::wstring_view message) noexcept {
#ifdef _DEBUG
    util::WriteLogLine(message);
    try {
        std::wstring output(message);
        output.push_back(L'\n');
        OutputDebugStringW(output.c_str());
    } catch (...) {
    }
#else
    try {
        auto line = util::details::FormatLogLine(message);
        util::details::LogTail().Add(line);
        if (ShouldPersistReleaseTrace(message)) {
            util::Logger::Instance().Enqueue(std::move(line));
        }
    } catch (...) {
    }
#endif
}

#ifdef _DEBUG
void DebugTraceDiagnostic(std::wstring_view message) noexcept {
    util::WriteLogLine(message);
    try {
        std::wstring output(message);
        output.push_back(L'\n');
        OutputDebugStringW(output.c_str());
    } catch (...) {
    }
}
#endif
