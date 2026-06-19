#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>

#include <control/CommandProtocol.hpp>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD c_initialPipeWaitMs = 250;
constexpr DWORD c_launchPipeWaitMs = 10000;
constexpr DWORD c_retryIntervalMs = 200;

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
  apc2ctl status [--json]
  apc2ctl list [--json]
  apc2ctl connect (--id ID | --name NAME | --mac MAC | --last | TARGET)
  apc2ctl disconnect (--id ID | --name NAME | --mac MAC | --last | TARGET)
  apc2ctl reconnect (--id ID | --name NAME | --mac MAC | --last | TARGET)
  apc2ctl toggle [--last | --id ID | --name NAME | --mac MAC | TARGET]
  apc2ctl disconnect-all
  apc2ctl reconnect-all

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
        } else if (EqualsIgnoreCase(arg, L"--last")) {
            request.Target = apc::control::TargetKind::Last;
            request.Payload.clear();
        } else if (EqualsIgnoreCase(arg, L"--id") || EqualsIgnoreCase(arg, L"--name") ||
                   EqualsIgnoreCase(arg, L"--mac")) {
            auto value = ReadOptionValue(i, argc, argv);
            if (!value) return Error(3, L"Missing value for " + std::wstring(arg) + L".\n");
            request.Payload = std::move(*value);
            if (EqualsIgnoreCase(arg, L"--id")) request.Target = apc::control::TargetKind::Id;
            if (EqualsIgnoreCase(arg, L"--name")) request.Target = apc::control::TargetKind::Name;
            if (EqualsIgnoreCase(arg, L"--mac")) request.Target = apc::control::TargetKind::Mac;
        } else if (!arg.empty() && arg.front() == L'-') {
            return Error(3, L"Unknown option: " + std::wstring(arg) + L"\n");
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
            request.Target = apc::control::TargetKind::Last;
        } else {
            return Error(3, L"A device target is required.\n");
        }
    }

    return {.Send = true, .Request = std::move(request)};
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
    if (EqualsIgnoreCase(command, L"status")) {
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
    } else {
        return Error(3, L"Unknown command: " + std::wstring(command) + L"\n\n" + HelpText());
    }

    for (int i = 2; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
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

std::filesystem::path CurrentExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD actual = 0;
    while (true) {
        actual = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (actual == 0) return {};
        if (actual < path.size()) break;
        path.resize(path.size() * 2);
    }
    path.resize(actual);
    return path;
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

bool LaunchLooseApp() {
    const auto controlPath = CurrentExecutablePath();
    if (controlPath.empty()) return false;

    std::vector<std::filesystem::path> candidates;
    const auto controlDir = controlPath.parent_path();
    candidates.push_back(controlDir / L"AudioPlaybackConnector2.exe");
    candidates.push_back(controlDir / L"AudioPlaybackConnector2" / L"AudioPlaybackConnector2.exe");
    candidates.push_back(controlDir.parent_path() / L"AudioPlaybackConnector2" / L"AudioPlaybackConnector2.exe");

    for (auto const& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) continue;

        std::wstring commandLine = L"\"" + candidate.wstring() + L"\"";
        STARTUPINFOW startupInfo{sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo{};
        if (CreateProcessW(candidate.c_str(),
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           DETACHED_PROCESS,
                           nullptr,
                           candidate.parent_path().c_str(),
                           &startupInfo,
                           &processInfo)) {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            return true;
        }
    }

    return false;
}

bool LaunchTrayApp() {
    return LaunchPackagedApp() || LaunchLooseApp();
}

bool TrySendOnce(apc::control::Request const& request, apc::control::Response& response, DWORD waitMs) {
    const auto pipeName = apc::control::PipeName();
    const auto deadline = GetTickCount64() + waitMs;

    while (true) {
        HANDLE pipe =
            CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto closePipe = [pipe]() { CloseHandle(pipe); };
            struct Guard {
                decltype(closePipe)& Close;
                ~Guard() { Close(); }
            } guard{closePipe};

            if (!apc::control::WriteRequest(pipe, request)) return false;
            return apc::control::ReadResponse(pipe, response);
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
    if (!LaunchTrayApp()) return false;

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
