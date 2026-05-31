#pragma once

#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger ////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

namespace details {
bool EnsureDirectory(std::filesystem::path const& path);
}

std::filesystem::path const& GetCachedLogPath();
void WriteLogLine(std::wstring_view message) noexcept;

class Logger {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static Logger& Instance() noexcept;

    Logger(Logger const&) = delete;
    Logger& operator=(Logger const&) = delete;

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

inline void DebugTraceException(std::wstring_view context, winrt::hresult_error const& ex) noexcept {
    DebugTrace(L"{0}: 0x{1:08X} {2}", context, static_cast<uint32_t>(ex.code()), ex.message());
}

inline void DebugTraceException(std::wstring_view context, std::exception const& ex) noexcept {
    try {
        DebugTrace(L"{0}: {1}", context, Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"{0}: standard exception", context);
    }
}

inline void DebugTraceUnknownException(std::wstring_view context) noexcept {
    DebugTrace(L"{0}: unknown exception", context);
}

} // namespace util
