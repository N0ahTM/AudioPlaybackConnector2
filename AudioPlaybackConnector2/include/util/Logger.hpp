#pragma once

#include <util/Util.hpp>

#include <exception>
#include <format>
#include <memory>
#include <winrt/base.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger ////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

namespace details {
bool EnsureDirectory(std::filesystem::path const& path);
void AppendLogBytesDirect(std::string_view bytes) noexcept;
} // namespace details

std::filesystem::path const& GetCachedLogPath();
void WriteLogLine(std::wstring_view message) noexcept;
void FlushInMemoryLogTailToFile(std::wstring_view reason, std::uint32_t exceptionCode = 0) noexcept;

class Logger {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static Logger& Instance() noexcept;

    Logger(Logger const&) = delete;
    Logger& operator=(Logger const&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    ~Logger() noexcept;

    void Enqueue(std::string message) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct Impl;

    Logger() noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace util

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Debug Logging /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DebugTrace(std::wstring_view message) noexcept;

template <typename... Args> inline void DebugTrace(std::wstring_view fmt, Args&&... args) noexcept {
    try {
        DebugTrace(std::vformat(fmt, std::make_wformat_args(args...)));
    } catch (...) {
        DebugTrace(fmt);
    }
}

namespace util {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Stack Trace ///////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Exception Helpers /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline void DebugTraceException(std::wstring_view context, winrt::hresult_error const& ex) noexcept {
    DebugTrace(L"{0}: 0x{1:08X} {2}", context, static_cast<uint32_t>(ex.code()), ex.message());
    DebugTrace(L"{0} stack: {1}", context, CaptureCurrentStackTrace());
}

inline void DebugTraceException(std::wstring_view context, std::exception const& ex) noexcept {
    try {
        DebugTrace(L"{0}: {1}", context, Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"{0}: standard exception", context);
    }
    DebugTrace(L"{0} stack: {1}", context, CaptureCurrentStackTrace());
}

inline void DebugTraceUnknownException(std::wstring_view context) noexcept {
    DebugTrace(L"{0}: unknown exception", context);
    DebugTrace(L"{0} stack: {1}", context, CaptureCurrentStackTrace());
}

} // namespace util
