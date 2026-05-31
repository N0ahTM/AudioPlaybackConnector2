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
