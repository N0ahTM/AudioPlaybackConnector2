#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>

#include <control/CommandProtocol.hpp>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD c_initialPipeWaitMs = 250;
constexpr DWORD c_launchPipeWaitMs = 10000;
constexpr DWORD c_retryIntervalMs = 200;
constexpr DWORD c_pipeExchangeTimeoutMs = 35000;

struct ParseResult {
    bool Send = false;
    apc::control::Request Request;
    uint32_t ExitCode = 0;
    std::wstring Message;
};

std::wstring_view ToView(wchar_t const* value) {
    return value ? std::wstring_view(value) : std::wstring_view();
}

bool EqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](wchar_t a, wchar_t b) {
               return towlower(a) == towlower(b);
           });
}

std::string Utf16ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size,
                        nullptr,
                        nullptr);
    return output;
}

void WriteStream(DWORD streamId, std::wstring_view text) {
    auto handle = GetStdHandle(streamId);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }

    auto utf8 = Utf16ToUtf8(text);
    if (utf8.empty()) return;
    DWORD written = 0;
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void WriteStdout(std::wstring_view text) {
    WriteStream(STD_OUTPUT_HANDLE, text);
}

void WriteStderr(std::wstring_view text) {
    WriteStream(STD_ERROR_HANDLE, text);
}

std::wstring HelpText() {
    return LR"(AudioPlaybackConnector2 command line control

Usage:
  apc2ctl show
  apc2ctl settings
  apc2ctl status [--json]
  apc2ctl list [--json]
  apc2ctl connect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl disconnect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl reconnect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl toggle [--last | --default | --id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET]
  apc2ctl disconnect-all
  apc2ctl reconnect-all
  apc2ctl default show [--json] [--raw]
  apc2ctl default set (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET)
  apc2ctl default clear
  apc2ctl alias list [--json] [--raw]
  apc2ctl alias set (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET) (--value VALUE | VALUE)
  apc2ctl alias clear (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET)

TARGET is resolved as an exact device ID, a MAC address contained in the device ID, then a device name.
)";
}

ParseResult Error(uint32_t exitCode, std::wstring message) {
    return {.Send = false, .ExitCode = exitCode, .Message = std::move(message)};
}

std::optional<std::wstring> ReadOptionValue(int& index, int argc, wchar_t** argv) {
    if (index + 1 >= argc) return std::nullopt;
    auto value = ToView(argv[index + 1]);
    if (!value.empty() && value.front() == L'-') return std::nullopt;
    ++index;
    return std::wstring(value);
}

ParseResult ParseTargetOptions(apc::control::CommandType command, int startIndex, int argc, wchar_t** argv) {
    apc::control::Request request;
    request.Command = command;
    request.Target = apc::control::TargetKind::None;

    std::optional<std::wstring> positionalTarget;
    for (int i = startIndex; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
        } else if (EqualsIgnoreCase(arg, L"--raw")) {
            request.Flags |= apc::control::CommandFlagRaw;
        } else if (EqualsIgnoreCase(arg, L"--last")) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            request.Target = apc::control::TargetKind::Last;
            request.Payload.clear();
        } else if (EqualsIgnoreCase(arg, L"--default")) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            request.Target = apc::control::TargetKind::Default;
            request.Payload.clear();
        } else if (EqualsIgnoreCase(arg, L"--id") || EqualsIgnoreCase(arg, L"--name") ||
                   EqualsIgnoreCase(arg, L"--mac") || EqualsIgnoreCase(arg, L"--alias")) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            auto value = ReadOptionValue(i, argc, argv);
            if (!value) return Error(3, L"Missing value for " + std::wstring(arg) + L".\n");
            request.Payload = std::move(*value);
            if (EqualsIgnoreCase(arg, L"--id")) request.Target = apc::control::TargetKind::Id;
            if (EqualsIgnoreCase(arg, L"--name")) request.Target = apc::control::TargetKind::Name;
            if (EqualsIgnoreCase(arg, L"--mac")) request.Target = apc::control::TargetKind::Mac;
            if (EqualsIgnoreCase(arg, L"--alias")) request.Target = apc::control::TargetKind::Alias;
        } else if (!arg.empty() && arg.front() == L'-') {
            return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
        } else if (request.Target != apc::control::TargetKind::None) {
            return Error(3, L"A positional target cannot be combined with a target selector.\n");
        } else if (!positionalTarget) {
            positionalTarget = std::wstring(arg);
        } else {
            return Error(3, L"Only one positional target is supported.\n");
        }
    }

    if (request.Target == apc::control::TargetKind::None && positionalTarget) {
        request.Target = apc::control::TargetKind::Auto;
        request.Payload = std::move(*positionalTarget);
    }

    if (request.Target == apc::control::TargetKind::None) {
        if (command == apc::control::CommandType::ToggleLast) {
            request.Target = apc::control::TargetKind::Default;
        } else {
            return Error(3, L"A device target is required.\n");
        }
    }

    return {.Send = true, .Request = std::move(request)};
}

ParseResult ParseDefaultCommand(int startIndex, int argc, wchar_t** argv) {
    if (startIndex >= argc) return Error(3, L"default requires show, set, or clear.\n");

    auto subcommand = ToView(argv[startIndex]);
    if (EqualsIgnoreCase(subcommand, L"show")) {
        apc::control::Request request;
        request.Command = apc::control::CommandType::DefaultShow;
        for (int i = startIndex + 1; i < argc; ++i) {
            auto arg = ToView(argv[i]);
            if (EqualsIgnoreCase(arg, L"--json")) {
                request.Flags |= apc::control::CommandFlagJson;
            } else if (EqualsIgnoreCase(arg, L"--raw")) {
                request.Flags |= apc::control::CommandFlagRaw;
            } else {
                return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
            }
        }
        return {.Send = true, .Request = std::move(request)};
    }

    if (EqualsIgnoreCase(subcommand, L"clear")) {
        apc::control::Request request;
        request.Command = apc::control::CommandType::DefaultClear;
        for (int i = startIndex + 1; i < argc; ++i) {
            auto arg = ToView(argv[i]);
            if (EqualsIgnoreCase(arg, L"--json")) {
                request.Flags |= apc::control::CommandFlagJson;
            } else if (EqualsIgnoreCase(arg, L"--raw")) {
                request.Flags |= apc::control::CommandFlagRaw;
            } else {
                return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
            }
        }
        return {.Send = true, .Request = std::move(request)};
    }

    if (EqualsIgnoreCase(subcommand, L"set")) {
        return ParseTargetOptions(apc::control::CommandType::DefaultSet, startIndex + 1, argc, argv);
    }

    return Error(3, L"Unknown default command: " + std::wstring(subcommand) + L"\n");
}

ParseResult ParseAliasSetOptions(int startIndex, int argc, wchar_t** argv) {
    apc::control::Request request;
    request.Command = apc::control::CommandType::AliasSet;
    request.Target = apc::control::TargetKind::None;

    std::optional<std::wstring> positionalTarget;
    std::optional<std::wstring> alias;
    for (int i = startIndex; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
        } else if (EqualsIgnoreCase(arg, L"--raw")) {
            request.Flags |= apc::control::CommandFlagRaw;
        } else if (EqualsIgnoreCase(arg, L"--value")) {
            auto value = ReadOptionValue(i, argc, argv);
            if (!value) return Error(3, L"Missing value for --value.\n");
            if (alias) return Error(3, L"Only one alias value is supported.\n");
            alias = std::move(*value);
        } else if (EqualsIgnoreCase(arg, L"--id") || EqualsIgnoreCase(arg, L"--name") ||
                   EqualsIgnoreCase(arg, L"--mac") || EqualsIgnoreCase(arg, L"--alias")) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            auto value = ReadOptionValue(i, argc, argv);
            if (!value) return Error(3, L"Missing value for " + std::wstring(arg) + L".\n");
            request.Payload = std::move(*value);
            if (EqualsIgnoreCase(arg, L"--id")) request.Target = apc::control::TargetKind::Id;
            if (EqualsIgnoreCase(arg, L"--name")) request.Target = apc::control::TargetKind::Name;
            if (EqualsIgnoreCase(arg, L"--mac")) request.Target = apc::control::TargetKind::Mac;
            if (EqualsIgnoreCase(arg, L"--alias")) request.Target = apc::control::TargetKind::Alias;
        } else if (!arg.empty() && arg.front() == L'-') {
            return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
        } else if (request.Target == apc::control::TargetKind::None && !positionalTarget) {
            positionalTarget = std::wstring(arg);
        } else if (!alias) {
            alias = std::wstring(arg);
        } else {
            return Error(3, L"Only one alias value is supported.\n");
        }
    }

    if (request.Target == apc::control::TargetKind::None && positionalTarget) {
        request.Target = apc::control::TargetKind::Auto;
        request.Payload = std::move(*positionalTarget);
    }
    if (request.Target == apc::control::TargetKind::None || request.Payload.empty()) {
        return Error(3, L"A device target is required.\n");
    }
    if (!alias) return Error(3, L"An alias value is required.\n");

    request.Payload += L"\n";
    request.Payload += *alias;
    return {.Send = true, .Request = std::move(request)};
}

ParseResult ParseAliasCommand(int startIndex, int argc, wchar_t** argv) {
    if (startIndex >= argc) return Error(3, L"alias requires list, set, or clear.\n");

    auto subcommand = ToView(argv[startIndex]);
    if (EqualsIgnoreCase(subcommand, L"list")) {
        apc::control::Request request;
        request.Command = apc::control::CommandType::AliasList;
        for (int i = startIndex + 1; i < argc; ++i) {
            auto arg = ToView(argv[i]);
            if (EqualsIgnoreCase(arg, L"--json")) {
                request.Flags |= apc::control::CommandFlagJson;
            } else if (EqualsIgnoreCase(arg, L"--raw")) {
                request.Flags |= apc::control::CommandFlagRaw;
            } else {
                return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
            }
        }
        return {.Send = true, .Request = std::move(request)};
    }

    if (EqualsIgnoreCase(subcommand, L"set")) {
        return ParseAliasSetOptions(startIndex + 1, argc, argv);
    }

    if (EqualsIgnoreCase(subcommand, L"clear")) {
        return ParseTargetOptions(apc::control::CommandType::AliasClear, startIndex + 1, argc, argv);
    }

    return Error(3, L"Unknown alias command: " + std::wstring(subcommand) + L"\n");
}

ParseResult ParseCommandLine(int argc, wchar_t** argv) {
    if (argc <= 1) {
        return Error(0, HelpText());
    }

    auto command = ToView(argv[1]);
    if (EqualsIgnoreCase(command, L"help") || EqualsIgnoreCase(command, L"--help") ||
        EqualsIgnoreCase(command, L"-h")) {
        return Error(0, HelpText());
    }

    apc::control::Request request;
    if (EqualsIgnoreCase(command, L"show")) {
        request.Command = apc::control::CommandType::Show;
    } else if (EqualsIgnoreCase(command, L"settings")) {
        request.Command = apc::control::CommandType::Settings;
    } else if (EqualsIgnoreCase(command, L"status")) {
        request.Command = apc::control::CommandType::Status;
    } else if (EqualsIgnoreCase(command, L"list")) {
        request.Command = apc::control::CommandType::List;
    } else if (EqualsIgnoreCase(command, L"disconnect-all")) {
        request.Command = apc::control::CommandType::DisconnectAll;
    } else if (EqualsIgnoreCase(command, L"reconnect-all")) {
        request.Command = apc::control::CommandType::ReconnectAll;
    } else if (EqualsIgnoreCase(command, L"connect")) {
        return ParseTargetOptions(apc::control::CommandType::Connect, 2, argc, argv);
    } else if (EqualsIgnoreCase(command, L"disconnect")) {
        return ParseTargetOptions(apc::control::CommandType::Disconnect, 2, argc, argv);
    } else if (EqualsIgnoreCase(command, L"reconnect")) {
        return ParseTargetOptions(apc::control::CommandType::Reconnect, 2, argc, argv);
    } else if (EqualsIgnoreCase(command, L"toggle")) {
        return ParseTargetOptions(apc::control::CommandType::ToggleLast, 2, argc, argv);
    } else if (EqualsIgnoreCase(command, L"default")) {
        return ParseDefaultCommand(2, argc, argv);
    } else if (EqualsIgnoreCase(command, L"alias")) {
        return ParseAliasCommand(2, argc, argv);
    } else {
        return Error(3, L"Unknown command: " + std::wstring(command) + L"\n\n" + HelpText());
    }

    for (int i = 2; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
        } else if (EqualsIgnoreCase(arg, L"--raw")) {
            request.Flags |= apc::control::CommandFlagRaw;
        } else {
            return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
        }
    }

    return {.Send = true, .Request = std::move(request)};
}

std::optional<std::wstring> CurrentPackageFamilyName() {
    UINT32 length = 0;
    const LONG initial = GetCurrentPackageFamilyName(&length, nullptr);
    if (initial != ERROR_INSUFFICIENT_BUFFER || length == 0) return std::nullopt;

    std::wstring familyName(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, familyName.data()) != ERROR_SUCCESS || length == 0) return std::nullopt;
    familyName.resize(length - 1);
    return familyName;
}

bool LaunchPackagedApp() {
    auto familyName = CurrentPackageFamilyName();
    if (!familyName) return false;

    auto target = L"shell:AppsFolder\\" + *familyName + L"!App";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) == TRUE;
}

bool TrySendOnce(apc::control::Request const& request, apc::control::Response& response, DWORD waitMs) {
    const auto pipeName = apc::control::PipeName();
    const auto deadline = GetTickCount64() + waitMs;

    while (true) {
        HANDLE pipe = CreateFileW(
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto closePipe = [pipe]() { CloseHandle(pipe); };
            struct Guard {
                decltype(closePipe)& Close;
                ~Guard() { Close(); }
            } guard{closePipe};

            const auto exchangeDeadline = apc::control::DeadlineAfter(c_pipeExchangeTimeoutMs);
            if (apc::control::WriteRequest(pipe, request, nullptr, exchangeDeadline) !=
                apc::control::IoStatus::Success) {
                return false;
            }
            return apc::control::ReadResponse(pipe, response, nullptr, exchangeDeadline) ==
                   apc::control::IoStatus::Success;
        }

        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return false;
        if (GetTickCount64() >= deadline) return false;
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), std::min<DWORD>(c_retryIntervalMs, waitMs));
        } else {
            Sleep(c_retryIntervalMs);
        }
    }
}

bool SendRequest(apc::control::Request const& request, apc::control::Response& response) {
    if (TrySendOnce(request, response, c_initialPipeWaitMs)) return true;

    if (request.Command == apc::control::CommandType::Status) return false;
    if (!LaunchPackagedApp()) return false;

    return TrySendOnce(request, response, c_launchPipeWaitMs);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    auto parsed = ParseCommandLine(argc, argv);
    if (!parsed.Send) {
        if (parsed.ExitCode == 0) {
            WriteStdout(parsed.Message);
        } else {
            WriteStderr(parsed.Message);
        }
        return static_cast<int>(parsed.ExitCode);
    }

    apc::control::Response response;
    if (!SendRequest(parsed.Request, response)) {
        WriteStderr(L"AudioPlaybackConnector2 is not running or did not accept the command.\n");
        return static_cast<int>(apc::control::ExitCode::Unavailable);
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
