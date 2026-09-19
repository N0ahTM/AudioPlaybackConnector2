#include <windows.h>
#include <bcrypt.h>

#include <control/CommandClient.hpp>
#include <control/CliParser.hpp>
#include <control/Win32CommandTransport.hpp>
#include <control/CommandProtocol.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

std::string Utf16ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(size, '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return output;
}

void WriteStream(DWORD streamId, std::wstring_view text) {
    auto handle = GetStdHandle(streamId);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        while (!text.empty()) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteConsoleW(handle, text.data(), chunk, &written, nullptr) || written == 0) return;
            text.remove_prefix(written);
        }
        return;
    }

    auto utf8 = Utf16ToUtf8(text);
    if (utf8.empty()) return;
    std::string_view remaining(utf8);
    while (!remaining.empty()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(handle, remaining.data(), chunk, &written, nullptr) || written == 0) return;
        remaining.remove_prefix(written);
    }
}

void WriteStdout(std::wstring_view text) {
    WriteStream(STD_OUTPUT_HANDLE, text);
}

void WriteStderr(std::wstring_view text) {
    WriteStream(STD_ERROR_HANDLE, text);
}

apc::control::CorrelationId CreateCorrelationId() noexcept {
    apc::control::CorrelationId value;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&value),
                        static_cast<ULONG>(sizeof(value)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 &&
        !value.Empty()) {
        return value;
    }

    static std::atomic_uint64_t fallbackSequence = 0;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    value.High = static_cast<uint64_t>(counter.QuadPart) ^ GetTickCount64() ^
                 (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    value.Low = ++fallbackSequence ^ (static_cast<uint64_t>(GetCurrentThreadId()) << 32);
    if (value.Empty()) value.Low = 1;
    return value;
}

} // namespace

int Run(int argc, wchar_t** argv, bool jsonRequestedOnError, apc::control::ExitCode& unexpectedFailureCode) {
    auto parsed = apc::control::cli::ParseCommandLine(argc, argv);
    const bool jsonRequested =
        parsed.Send ? (parsed.Request.Flags & apc::control::CommandFlagJson) != 0 : jsonRequestedOnError;
    if (!parsed.Send) {
        if (parsed.ExitCode == 0) {
            WriteStdout(parsed.Message);
        } else {
            const auto code = static_cast<apc::control::ExitCode>(parsed.ExitCode);
            unexpectedFailureCode = code;
            WriteStderr(apc::control::cli::LocalError(jsonRequested, code, parsed.Message));
        }
        return static_cast<int>(parsed.ExitCode);
    }

    parsed.Request.CorrelationId = CreateCorrelationId();
    apc::control::Response response;
    apc::control::client::Win32CommandTransport transport;
    unexpectedFailureCode = apc::control::ExitCode::Indeterminate;
    const auto sendResult = apc::control::client::SendRequest(transport, parsed.Request, response);
    if (sendResult == apc::control::client::SendResult::InvalidRequest) {
        unexpectedFailureCode = apc::control::ExitCode::InvalidRequest;
        WriteStderr(apc::control::cli::LocalError(
            jsonRequested, unexpectedFailureCode, L"The command request is invalid and was not sent.\n"));
        return static_cast<int>(apc::control::ExitCode::InvalidRequest);
    }
    if (sendResult == apc::control::client::SendResult::Indeterminate) {
        WriteStderr(apc::control::cli::LocalError(
            jsonRequested,
            unexpectedFailureCode,
            L"The command may have completed, but the result could not be confirmed. It was not sent to a different "
            L"app instance.\n"));
        return static_cast<int>(apc::control::ExitCode::Indeterminate);
    }
    if (sendResult != apc::control::client::SendResult::Complete) {
        unexpectedFailureCode = apc::control::ExitCode::Unavailable;
        WriteStderr(
            apc::control::cli::LocalError(jsonRequested,
                                          unexpectedFailureCode,
                                          L"AudioPlaybackConnector2 is not running or did not accept the command.\n"));
        return static_cast<int>(apc::control::ExitCode::Unavailable);
    }

    unexpectedFailureCode = response.Code;
    if (response.Payload.empty() && response.Code != apc::control::ExitCode::Success) {
        WriteStderr(apc::control::cli::LocalError(
            jsonRequested, response.Code, L"The command result did not include a diagnostic message.\n"));
    } else if (response.Payload.empty() && jsonRequested) {
        WriteStdout(L"{\"ok\":true,\"exitCode\":0,\"message\":\"\"}\n");
    }
    if (!response.Payload.empty()) {
        if (response.Code == apc::control::ExitCode::Success) {
            WriteStdout(response.Payload);
        } else {
            WriteStderr(response.Payload);
        }
        if (response.Payload.back() != L'\n') {
            WriteStream(response.Code == apc::control::ExitCode::Success ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, L"\n");
        }
    }

    return static_cast<int>(response.Code);
}

int wmain(int argc, wchar_t** argv) noexcept {
    auto unexpectedFailureCode = apc::control::ExitCode::OperationFailed;
    bool jsonRequested = false;
    try {
        jsonRequested = apc::control::cli::JsonRequested(argc, argv);
        return Run(argc, argv, jsonRequested, unexpectedFailureCode);
    } catch (...) {
        if (unexpectedFailureCode == apc::control::ExitCode::Success) {
            unexpectedFailureCode = apc::control::ExitCode::OperationFailed;
        }
        constexpr std::string_view textMessage =
            "AudioPlaybackConnector2.Control encountered an unexpected local failure.\r\n";
        const auto jsonMessage = [unexpectedFailureCode]() noexcept -> std::string_view {
            switch (unexpectedFailureCode) {
                case apc::control::ExitCode::InvalidRequest:
                    return "{\"ok\":false,\"exitCode\":3,\"message\":\"Unexpected local failure.\"}\r\n";
                case apc::control::ExitCode::NotFound:
                    return "{\"ok\":false,\"exitCode\":4,\"message\":\"Unexpected local failure.\"}\r\n";
                case apc::control::ExitCode::Ambiguous:
                    return "{\"ok\":false,\"exitCode\":5,\"message\":\"Unexpected local failure.\"}\r\n";
                case apc::control::ExitCode::Unavailable:
                    return "{\"ok\":false,\"exitCode\":7,\"message\":\"Unexpected local failure.\"}\r\n";
                case apc::control::ExitCode::Busy:
                    return "{\"ok\":false,\"exitCode\":8,\"message\":\"Unexpected local failure.\"}\r\n";
                case apc::control::ExitCode::Indeterminate:
                    return "{\"ok\":false,\"exitCode\":9,\"message\":\"Unexpected local failure.\"}\r\n";
                default: return "{\"ok\":false,\"exitCode\":6,\"message\":\"Unexpected local failure.\"}\r\n";
            }
        }();
        auto message = jsonRequested ? jsonMessage : textMessage;
        auto error = GetStdHandle(STD_ERROR_HANDLE);
        if (error && error != INVALID_HANDLE_VALUE) {
            while (!message.empty()) {
                DWORD written = 0;
                if (!WriteFile(error, message.data(), static_cast<DWORD>(message.size()), &written, nullptr) ||
                    written == 0) {
                    break;
                }
                message.remove_prefix(written);
            }
        }
        return static_cast<int>(unexpectedFailureCode);
    }
}
