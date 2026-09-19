#pragma once

#include <control/CommandProtocol.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace apc::control::cli {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Command Line Contract /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct ParseResult {
    bool Send = false;
    apc::control::Request Request;
    std::uint32_t ExitCode = 0;
    std::wstring Message;
};

// argv includes the executable name. Parsing performs no I/O or process activation.
[[nodiscard]] ParseResult ParseCommandLine(int argc, wchar_t const* const* argv);
[[nodiscard]] bool JsonRequested(int argc, wchar_t const* const* argv) noexcept;
[[nodiscard]] std::wstring LocalError(bool jsonRequested, ExitCode code, std::wstring_view message);

} // namespace apc::control::cli
