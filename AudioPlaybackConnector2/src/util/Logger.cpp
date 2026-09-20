#include <util/Logger.hpp>
#include <util/Text.hpp>
#include <sal.h>
#include <concurrencysal.h>

#define SPDLOG_WCHAR_FILENAMES
#define SPDLOG_DISABLE_DEFAULT_LOGGER
#define SPDLOG_USE_STD_FORMAT
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <appmodel.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <utility>
#include <wil/resource.h>

namespace util {

namespace details {
inline std::wstring CaptureCurrentStackTrace(std::size_t skip = 2, std::size_t maxFrames = 12) noexcept {
    try {
        void* stack[32]{};
        auto frames = CaptureStackBackTrace(static_cast<DWORD>(skip), static_cast<DWORD>(maxFrames), stack, nullptr);
        if (frames == 0) return L"<empty>";
        std::wstring result;
        for (std::size_t i = 0; i < frames; ++i) {
            if (!result.empty()) result += L" <- ";
            result += std::format(L"0x{:X}", reinterpret_cast<std::uintptr_t>(stack[i]));
        }
        return result;
    } catch (...) {
        return L"<stacktrace failed>";
    }
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
/*//////// Logger Implementation //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace details {

// Storage is allocated during ordinary startup. Dump uses only this fixed
// scratch space, a nonblocking tail snapshot and native file operations.
struct EmergencyState {
    struct Line {
        std::array<char, 1024> Bytes{};
        std::uint16_t Length = 0;
    };

    explicit EmergencyState(std::filesystem::path path) : Path(std::move(path)) {}

    _Requires_lock_not_held_(TailLock) void Add(std::string_view text) noexcept {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            text.remove_suffix(1);
        const bool truncated = text.size() > 1022;
        auto count = std::min<std::size_t>(text.size(), truncated ? 1019 : 1022);
        if (count < text.size()) {
            while (count && (static_cast<unsigned char>(text[count]) & 0xC0) == 0x80)
                --count;
        }
        AcquireSRWLockExclusive(&TailLock);
        auto& line = Lines[Next];
        std::memcpy(line.Bytes.data(), text.data(), count);
        if (truncated) {
            std::memcpy(line.Bytes.data() + count, "...", 3);
            count += 3;
        }
        line.Bytes[count++] = '\r';
        line.Bytes[count++] = '\n';
        line.Length = static_cast<std::uint16_t>(count);
        Next = (Next + 1) % Lines.size();
        Count = std::min(Count + 1, Lines.size());
        ReleaseSRWLockExclusive(&TailLock);
    }

    _Requires_lock_not_held_(TailLock) bool Snapshot(std::size_t& count) noexcept {
        if (!TryAcquireSRWLockExclusive(&TailLock)) return false;
        count = Count;
        const auto first = Count == Lines.size() ? Next : 0;
        for (std::size_t index = 0; index < count; ++index)
            Scratch[index] = Lines[(first + index) % Lines.size()];
        ReleaseSRWLockExclusive(&TailLock);
        return true;
    }

    const std::filesystem::path Path;
    // Leaf lock: only the ring buffer; scratch output is owned by the Dumping gate.
    SRWLOCK TailLock = SRWLOCK_INIT;
    _Guarded_by_(TailLock) std::array<Line, 100> Lines {};
    std::array<Line, 100> Scratch{};
    _Guarded_by_(TailLock) std::size_t Next = 0;
    _Guarded_by_(TailLock) std::size_t Count = 0;
    std::atomic_flag Dumping = ATOMIC_FLAG_INIT;
};

bool WriteEmergencyBytes(HANDLE file, std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data(), size, &written, nullptr) || written == 0) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

wil::unique_hfile OpenEmergencyFile(std::filesystem::path const& path) noexcept {
    return wil::unique_hfile(CreateFileW(path.c_str(),
                                         FILE_APPEND_DATA,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr,
                                         OPEN_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr));
}

struct LogState {
    explicit LogState(std::filesystem::path path)
        : Path(path), Emergency(std::make_shared<EmergencyState>(std::move(path))) {}
    const std::filesystem::path Path;
    std::shared_ptr<EmergencyState> Emergency;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::size_t ActiveCalls = 0;
    bool Closing = false;
    bool Finished = false;
    std::atomic_size_t Dropped = 0;
    std::atomic_size_t Errors = 0;
    std::shared_ptr<spdlog::details::thread_pool> Pool;
    std::shared_ptr<spdlog::async_logger> Async;
    std::shared_ptr<spdlog::sinks::sink> File;

    void ObserveDrops(std::size_t count) noexcept {
        auto previous = Dropped.load(std::memory_order_relaxed);
        while (previous < count && !Dropped.compare_exchange_weak(previous, count, std::memory_order_relaxed)) {
        }
    }
};

// The callback owns everything needed by a still-running worker. It never
// references the Logger owner, its caller's stack or another product service.
struct LogRelease {
    std::shared_ptr<LogState> State;
    PTP_WORK Work = nullptr;

    ~LogRelease() {
        if (Work) CloseThreadpoolWork(Work);
    }

    static void CALLBACK Run(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK) noexcept {
        (void)CallbackMayRunLong(instance);
        std::unique_ptr<LogRelease> release(static_cast<LogRelease*>(context));
        auto state = release->State;
        {
            std::unique_lock lock(state->Mutex);
            state->Changed.wait(lock, [&] { return state->ActiveCalls == 0; });
        }
        state->ObserveDrops(state->Pool->overrun_counter());
        state->Pool.reset(); // Drains the queue and joins only on this callback.
        state->Async.reset();
        try {
            spdlog::logger finalWriter("shutdown", state->File);
            finalWriter.set_error_handler(
                [state](std::string const&) { state->Errors.fetch_add(1, std::memory_order_relaxed); });
            if (auto dropped = state->Dropped.load(); dropped != 0)
                finalWriter.warn("[Logger] Dropped {} queued log item(s) due to backpressure", dropped);
            finalWriter.flush();
        } catch (...) {
            // Logging is best effort; failure must still complete shutdown.
            state->Errors.fetch_add(1, std::memory_order_relaxed);
        }
        state->File.reset();
        release.reset();
        {
            std::lock_guard lock(state->Mutex);
            state->Finished = true;
        }
        state->Changed.notify_all();
    }
};

void SubmitLog(std::weak_ptr<LogState> const& weak,
               std::wstring_view message,
               bool flush,
               bool persist = true) noexcept {
    auto state = weak.lock();
    if (!state) return;
    {
        std::lock_guard lock(state->Mutex);
        if (state->Closing) return;
        ++state->ActiveCalls;
    }
    auto complete = wil::scope_exit([&] {
        state->ObserveDrops(state->Pool->overrun_counter());
        {
            std::lock_guard lock(state->Mutex);
            --state->ActiveCalls;
        }
        state->Changed.notify_all();
    });
    try {
        if (flush)
            state->Async->flush();
        else {
            state->Emergency->Add(FormatLogLine(message));
            if (persist) state->Async->info("{}", Utf16ToUtf8(message));
#ifdef _DEBUG
            auto output = std::wstring(message) + L"\n";
            OutputDebugStringW(output.c_str());
#endif
        }
    } catch (...) {
        // Encoding/allocation failure cannot escape a diagnostic call.
        state->Errors.fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace details

struct Logger::Impl {
    explicit Impl(std::filesystem::path const& path, std::size_t capacity, std::size_t rotationBytes)
        : State(std::make_shared<details::LogState>(path)), Release(std::make_unique<details::LogRelease>()) {
        if (capacity == 0 || rotationBytes == 0) throw std::invalid_argument("Invalid logger limits");
        Release->State = State;
        Release->Work = CreateThreadpoolWork(details::LogRelease::Run, Release.get(), nullptr);
        THROW_LAST_ERROR_IF(!Release->Work);
        // Allocate cleanup before opening a file or starting any worker.
        State->File = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path.native(), rotationBytes, 1);
        State->Pool = std::make_shared<spdlog::details::thread_pool>(capacity, 1);
        State->Async = std::make_shared<spdlog::async_logger>(
            "application", State->File, State->Pool, spdlog::async_overflow_policy::overrun_oldest);
        State->Async->set_pattern("%Y-%m-%d %H:%M:%S.%e [T:%t] %v");
        State->Async->set_error_handler([weak = std::weak_ptr(State)](std::string const&) {
            if (auto state = weak.lock()) state->Errors.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::shared_ptr<details::LogState> State;
    std::unique_ptr<details::LogRelease> Release;
};

Logger::Logger(std::filesystem::path path, std::size_t queueCapacity, std::size_t rotationBytes)
    : m_impl(std::make_unique<Impl>(path, queueCapacity, rotationBytes)) {}

Logger::~Logger() noexcept {
    (void)Shutdown();
}

LogSink Logger::Sink() const noexcept {
    return m_impl ? LogSink(m_impl->State) : LogSink{};
}

EmergencyLog Logger::Emergency() const noexcept {
    return m_impl ? EmergencyLog(m_impl->State->Emergency) : EmergencyLog{};
}

std::filesystem::path EmergencyLog::Path() const {
    return m_state ? m_state->Path : std::filesystem::path{};
}

bool EmergencyLog::Write(std::string_view bytes) const noexcept {
    if (!m_state || m_state->Path.empty()) return false;
    auto file = details::OpenEmergencyFile(m_state->Path);
    return file && details::WriteEmergencyBytes(file.get(), bytes);
}

bool EmergencyLog::Dump(std::wstring_view reason, std::uint32_t exceptionCode) const noexcept {
    if (!m_state || m_state->Path.empty() || m_state->Dumping.test_and_set(std::memory_order_acquire)) return false;
    auto finish = wil::scope_exit([&] { m_state->Dumping.clear(std::memory_order_release); });
    std::size_t count = 0;
    const bool available = m_state->Snapshot(count);
    auto file = details::OpenEmergencyFile(m_state->Path);
    if (!file) return false;
    wchar_t header[512]{};
    const int length = swprintf_s(header,
                                  L"\r\n[Logger] BEGIN diagnostic tail reason=%.*ls exception=0x%08X\r\n",
                                  static_cast<int>(std::min<std::size_t>(reason.size(), 160)),
                                  reason.empty() ? L"" : reason.data(),
                                  exceptionCode);
    if (length <= 0) return false;
    char utf8[2048]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, header, length, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0 || !details::WriteEmergencyBytes(file.get(), {utf8, static_cast<std::size_t>(bytes)})) return false;
    if (!available) {
        if (!details::WriteEmergencyBytes(file.get(), "[Logger] Tail busy; snapshot skipped\r\n")) return false;
    } else {
        for (std::size_t index = 0; index < count; ++index) {
            auto const& line = m_state->Scratch[index];
            if (!details::WriteEmergencyBytes(file.get(), {line.Bytes.data(), line.Length})) return false;
        }
    }
    return details::WriteEmergencyBytes(file.get(), "[Logger] END diagnostic tail\r\n");
}

LogStatistics Logger::Statistics() const noexcept {
    if (!m_impl) return {};
    return {m_impl->State->Dropped.load(), m_impl->State->Errors.load()};
}

bool Logger::Shutdown(std::chrono::milliseconds timeout) noexcept {
    if (!m_impl) return true;
    auto const& state = m_impl->State;
    details::LogRelease* release = nullptr;
    {
        std::lock_guard lock(state->Mutex);
        if (!state->Closing) {
            state->Closing = true;
            release = m_impl->Release.release();
        }
    }
    if (release) SubmitThreadpoolWork(release->Work);
    std::unique_lock lock(state->Mutex);
    return state->Changed.wait_for(lock, timeout, [&] { return state->Finished; });
}

std::filesystem::path LogSink::Path() const {
    auto state = m_state.lock();
    return state ? state->Path : std::filesystem::path{};
}

void LogSink::Write(std::wstring_view message) const noexcept {
    details::SubmitLog(m_state, message, false);
}
void LogSink::Exception(std::wstring_view context, winrt::hresult_error const& error) const noexcept {
    Trace(L"{0}: 0x{1:08X} {2}", context, static_cast<std::uint32_t>(error.code()), error.message());
    Trace(L"{0} stack: {1}", context, details::CaptureCurrentStackTrace());
}
void LogSink::Exception(std::wstring_view context, std::exception const& error) const noexcept {
    try {
        Trace(L"{0}: {1}", context, Utf8ToUtf16(error.what()));
    } catch (...) {
        Trace(L"{0}: standard exception", context);
    }
    Trace(L"{0} stack: {1}", context, details::CaptureCurrentStackTrace());
}
void LogSink::UnknownException(std::wstring_view context) const noexcept {
    Trace(L"{0}: unknown exception", context);
    Trace(L"{0} stack: {1}", context, details::CaptureCurrentStackTrace());
}
void LogSink::RequestFlush() const noexcept {
    details::SubmitLog(m_state, {}, true);
}

Logger::Logger() noexcept {
    try {
        auto path = details::ResolveLogPath();
        if (!path.empty()) m_impl = std::make_unique<Impl>(path, 10000, 2 * 1024 * 1024);
    } catch (...) {
        // Failure to initialize diagnostics must not prevent application startup.
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
        const std::wstring lower = util::LowerInvariant(message);

        constexpr std::array<std::wstring_view, 9> keywords{
            L"error", L"failed", L"warning", L"crash", L"exception", L"denied", L"timeout", L"corrupt", L"unsupported"};
        return std::ranges::any_of(keywords, [&](auto keyword) { return lower.find(keyword) != std::wstring::npos; });
    } catch (...) {
        return true;
    }
}
} // namespace
#endif

void util::LogSink::Trace(std::wstring_view message) const noexcept {
#ifdef _DEBUG
    details::SubmitLog(m_state, message, false);
#else
    details::SubmitLog(m_state, message, false, ShouldPersistReleaseTrace(message));
#endif
}
