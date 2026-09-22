#pragma once

#include <util/Util.hpp>

#include <exception>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <winrt/base.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger ////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

namespace details {
struct LogState;
struct EmergencyState;
} // namespace details

// Owns only the emergency path and bounded tail, never an asynchronous worker.
class EmergencyLog {
public:
    EmergencyLog() = default;
    [[nodiscard]] bool Dump(std::wstring_view reason, std::uint32_t exceptionCode = 0) const noexcept;
    [[nodiscard]] bool Write(std::string_view bytes) const noexcept;
    [[nodiscard]] std::filesystem::path Path() const;

private:
    friend class Logger;
    explicit EmergencyLog(std::shared_ptr<details::EmergencyState> state) : m_state(std::move(state)) {}
    std::shared_ptr<details::EmergencyState> m_state;
};

class LogSink {
public:
    LogSink() = default;
    [[nodiscard]] std::filesystem::path Path() const;
    void Write(std::wstring_view message) const noexcept;
    void Trace(std::wstring_view message) const noexcept;
    void Exception(std::wstring_view context, winrt::hresult_error const& error) const noexcept;
    void Exception(std::wstring_view context, std::exception const& error) const noexcept;
    void UnknownException(std::wstring_view context) const noexcept;
    template <typename... Args> void Trace(std::wstring_view format, Args&&... args) const noexcept {
        try {
            Trace(std::vformat(format, std::make_wformat_args(args...)));
        } catch (...) {
            Trace(format);
        }
    }
    // Best-effort queue request; Shutdown is the acknowledged drain boundary.
    void RequestFlush() const noexcept;

private:
    friend class Logger;
    explicit LogSink(std::weak_ptr<details::LogState> state) : m_state(std::move(state)) {}
    std::weak_ptr<details::LogState> m_state;
};

struct LogStatistics {
    // Queue entries: records and explicit flush requests share overflow policy.
    std::size_t Dropped = 0;
    std::size_t Errors = 0;
};

class Logger {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    Logger(Logger const&) = delete;
    Logger& operator=(Logger const&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    Logger() noexcept;
    ~Logger() noexcept;

    explicit Logger(std::filesystem::path const& path,
                    std::size_t queueCapacity = 10000,
                    std::size_t rotationBytes = 2 * 1024 * 1024);
    [[nodiscard]] LogSink Sink() const noexcept;
    [[nodiscard]] EmergencyLog Emergency() const noexcept;
    [[nodiscard]] LogStatistics Statistics() const noexcept;
    // Closes admission once; repeated calls wait for the same owned cleanup.
    // False means the deadline elapsed, not that outstanding work was destroyed.
    [[nodiscard]] bool Shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace util
