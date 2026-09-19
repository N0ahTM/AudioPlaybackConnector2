#pragma once

#include <util/Logger.hpp>

#include <filesystem>
#include <memory>

namespace util::crash {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace details {
struct CrashContext;
}

// The application root owns the one process registration. Construction and
// destruction are serialized on its UI thread; native callbacks may run anywhere.
class CrashHandlers {
public:
    explicit CrashHandlers(EmergencyLog emergency);
    ~CrashHandlers() noexcept;
    CrashHandlers(CrashHandlers const&) = delete;
    CrashHandlers& operator=(CrashHandlers const&) = delete;

private:
    std::unique_ptr<details::CrashContext> m_context;
};

void CheckAndPromptCrashReports(std::filesystem::path const& logPath);

} // namespace util::crash
