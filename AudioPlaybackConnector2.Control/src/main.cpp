#include <windows.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <control/CommandClient.hpp>
#include <control/CommandPipeSecurity.hpp>
#include <control/CommandProtocol.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr DWORD c_retryIntervalMs = 200;
constexpr DWORD c_pipeExchangeTimeoutMs = 42000;

class UniqueProcessHandle {
public:
    UniqueProcessHandle() = default;
    explicit UniqueProcessHandle(HANDLE value) noexcept : m_value(value) {}
    ~UniqueProcessHandle() {
        if (m_value) CloseHandle(m_value);
    }
    UniqueProcessHandle(UniqueProcessHandle const&) = delete;
    UniqueProcessHandle& operator=(UniqueProcessHandle const&) = delete;
    UniqueProcessHandle(UniqueProcessHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    UniqueProcessHandle& operator=(UniqueProcessHandle&& other) noexcept {
        if (this != &other) {
            if (m_value) CloseHandle(m_value);
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return m_value; }

private:
    HANDLE m_value = nullptr;
};

struct Win32ServerIdentity final : apc::control::client::ServerIdentity {
    DWORD ProcessId = 0;
    FILETIME CreationTime{};
    UniqueProcessHandle Process;
};

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

Use -- before a target or alias value that begins with '-'. TARGET is resolved as an exact device ID, then an exact
alias or device name, then a MAC address contained in the device ID, and finally an alias/name substring. Equal-rank
matches are rejected as ambiguous; use an explicit selector to disambiguate.
)";
}

ParseResult Error(uint32_t exitCode, std::wstring message) {
    return {.Send = false, .ExitCode = exitCode, .Message = std::move(message)};
}

bool JsonRequested(int argc, wchar_t** argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        const auto argument = ToView(argv[i]);
        if (EqualsIgnoreCase(argument, L"--id") || EqualsIgnoreCase(argument, L"--name") ||
            EqualsIgnoreCase(argument, L"--mac") || EqualsIgnoreCase(argument, L"--alias") ||
            EqualsIgnoreCase(argument, L"--value")) {
            if (i + 1 < argc && ToView(argv[i + 1]) == L"--") {
                i += std::min(2, argc - i - 1);
            } else if (i + 1 < argc && (ToView(argv[i + 1]).empty() || ToView(argv[i + 1]).front() != L'-')) {
                ++i;
            }
            continue;
        }
        if (argument == L"--") return false;
        if (EqualsIgnoreCase(argument, L"--json")) return true;
    }
    return false;
}

std::optional<std::wstring> ExpectedUnpackagedServerPath() {
    DWORD capacity = 260;
    for (;;) {
        std::wstring modulePath(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), capacity);
        if (length == 0) return std::nullopt;
        if (length < capacity) {
            modulePath.resize(length);
            const auto separator = modulePath.find_last_of(L"\\/");
            if (separator == std::wstring::npos) return std::nullopt;
            modulePath.resize(separator + 1);
            modulePath += L"AudioPlaybackConnector2\\AudioPlaybackConnector2.exe";
            return modulePath;
        }
        if (capacity >= 32'768) return std::nullopt;
        capacity = std::min<DWORD>(capacity * 2, 32'768);
    }
}

void AppendJsonString(std::wstring& output, std::wstring_view value) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    output.push_back(L'\"');
    for (wchar_t character : value) {
        switch (character) {
            case L'\"': output += L"\\\""; break;
            case L'\\': output += L"\\\\"; break;
            case L'\b': output += L"\\b"; break;
            case L'\f': output += L"\\f"; break;
            case L'\n': output += L"\\n"; break;
            case L'\r': output += L"\\r"; break;
            case L'\t': output += L"\\t"; break;
            default:
                if (character < L' ') {
                    output += L"\\u";
                    output.push_back(hex[(character >> 12) & 0xF]);
                    output.push_back(hex[(character >> 8) & 0xF]);
                    output.push_back(hex[(character >> 4) & 0xF]);
                    output.push_back(hex[character & 0xF]);
                } else {
                    output.push_back(character);
                }
                break;
        }
    }
    output.push_back(L'\"');
}

std::wstring LocalError(bool jsonRequested, apc::control::ExitCode code, std::wstring_view message) {
    if (!jsonRequested) return std::wstring(message);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        message.remove_suffix(1);

    std::wstring output = L"{\"ok\":false,\"exitCode\":";
    output += std::to_wstring(static_cast<std::uint32_t>(code));
    output += L",\"message\":";
    AppendJsonString(output, message);
    output += L"}\n";
    return output;
}

std::optional<std::wstring> ReadOptionValue(int& index, int argc, wchar_t** argv) {
    if (index + 1 >= argc) return std::nullopt;
    auto value = ToView(argv[index + 1]);
    if (value == L"--") {
        if (index + 2 >= argc) return std::nullopt;
        index += 2;
        return std::wstring(ToView(argv[index]));
    }
    if (!value.empty() && value.front() == L'-') return std::nullopt;
    ++index;
    return std::wstring(value);
}

ParseResult ParseTargetOptions(apc::control::CommandType command, int startIndex, int argc, wchar_t** argv) {
    apc::control::Request request;
    request.Command = command;
    request.Target = apc::control::TargetKind::None;
    const bool allowsRelativeTarget =
        command == apc::control::CommandType::Connect || command == apc::control::CommandType::Disconnect ||
        command == apc::control::CommandType::Reconnect || command == apc::control::CommandType::ToggleLast;

    std::optional<std::wstring> positionalTarget;
    bool optionsEnded = false;
    for (int i = startIndex; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (!optionsEnded && arg == L"--") {
            optionsEnded = true;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--raw")) {
            request.Flags |= apc::control::CommandFlagRaw;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--last")) {
            if (!allowsRelativeTarget) return Error(3, L"This command requires an explicit device target.\n");
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            request.Target = apc::control::TargetKind::Last;
            request.Payload.clear();
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--default")) {
            if (!allowsRelativeTarget) return Error(3, L"This command requires an explicit device target.\n");
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            request.Target = apc::control::TargetKind::Default;
            request.Payload.clear();
        } else if (!optionsEnded && (EqualsIgnoreCase(arg, L"--id") || EqualsIgnoreCase(arg, L"--name") ||
                                     EqualsIgnoreCase(arg, L"--mac") || EqualsIgnoreCase(arg, L"--alias"))) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            auto value = ReadOptionValue(i, argc, argv);
            if (!value || value->empty()) return Error(3, L"Missing value for " + std::wstring(arg) + L".\n");
            request.Payload = std::move(*value);
            if (EqualsIgnoreCase(arg, L"--id")) request.Target = apc::control::TargetKind::Id;
            if (EqualsIgnoreCase(arg, L"--name")) request.Target = apc::control::TargetKind::Name;
            if (EqualsIgnoreCase(arg, L"--mac")) request.Target = apc::control::TargetKind::Mac;
            if (EqualsIgnoreCase(arg, L"--alias")) request.Target = apc::control::TargetKind::Alias;
        } else if (!optionsEnded && !arg.empty() && arg.front() == L'-') {
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
    if (request.Target != apc::control::TargetKind::Last && request.Target != apc::control::TargetKind::Default &&
        request.Payload.empty()) {
        return Error(3, L"A non-empty device target is required.\n");
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
    bool optionsEnded = false;
    for (int i = startIndex; i < argc; ++i) {
        auto arg = ToView(argv[i]);
        if (!optionsEnded && arg == L"--") {
            optionsEnded = true;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--json")) {
            request.Flags |= apc::control::CommandFlagJson;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--raw")) {
            request.Flags |= apc::control::CommandFlagRaw;
        } else if (!optionsEnded && EqualsIgnoreCase(arg, L"--value")) {
            auto value = ReadOptionValue(i, argc, argv);
            if (!value || value->empty()) return Error(3, L"Missing value for --value.\n");
            if (alias) return Error(3, L"Only one alias value is supported.\n");
            alias = std::move(*value);
        } else if (!optionsEnded && (EqualsIgnoreCase(arg, L"--id") || EqualsIgnoreCase(arg, L"--name") ||
                                     EqualsIgnoreCase(arg, L"--mac") || EqualsIgnoreCase(arg, L"--alias"))) {
            if (request.Target != apc::control::TargetKind::None || positionalTarget) {
                return Error(3, L"Only one device target selector is supported.\n");
            }
            auto value = ReadOptionValue(i, argc, argv);
            if (!value || value->empty()) return Error(3, L"Missing value for " + std::wstring(arg) + L".\n");
            request.Payload = std::move(*value);
            if (EqualsIgnoreCase(arg, L"--id")) request.Target = apc::control::TargetKind::Id;
            if (EqualsIgnoreCase(arg, L"--name")) request.Target = apc::control::TargetKind::Name;
            if (EqualsIgnoreCase(arg, L"--mac")) request.Target = apc::control::TargetKind::Mac;
            if (EqualsIgnoreCase(arg, L"--alias")) request.Target = apc::control::TargetKind::Alias;
        } else if (!optionsEnded && !arg.empty() && arg.front() == L'-') {
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
    if (!alias || alias->empty()) return Error(3, L"A non-empty alias value is required.\n");

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

bool TryLaunchPackagedApp() {
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

std::shared_ptr<Win32ServerIdentity>
CaptureServerIdentity(HANDLE pipe,
                      std::optional<apc::control::ExecutableFileIdentity> const& expectedUnpackagedServerIdentity) {
    ULONG processId = 0;
    if (!GetNamedPipeServerProcessId(pipe, &processId) || processId == 0) return {};

    UniqueProcessHandle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)));
    if (!process.Get()) return {};
    if (!apc::control::IsTrustedPeerProcess(
            process.Get(), static_cast<DWORD>(processId), expectedUnpackagedServerIdentity)) {
        return {};
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user) ||
        WaitForSingleObject(process.Get(), 0) != WAIT_TIMEOUT) {
        return {};
    }
    auto identity = std::make_shared<Win32ServerIdentity>();
    identity->ProcessId = static_cast<DWORD>(processId);
    identity->CreationTime = creation;
    identity->Process = std::move(process);
    return identity;
}

bool IsSameLiveServer(Win32ServerIdentity const& expected, Win32ServerIdentity const& observed) noexcept {
    return expected.ProcessId == observed.ProcessId &&
           CompareFileTime(&expected.CreationTime, &observed.CreationTime) == 0 && expected.Process.Get() &&
           WaitForSingleObject(expected.Process.Get(), 0) == WAIT_TIMEOUT;
}

apc::control::client::AttemptResult
TrySendOnce(apc::control::Request const& request,
            apc::control::Response& response,
            DWORD waitMs,
            std::uint64_t overallDeadline,
            std::wstring const& pipeName,
            std::optional<apc::control::ExecutableFileIdentity> const& expectedUnpackagedServerIdentity,
            std::shared_ptr<Win32ServerIdentity>& observedServer,
            Win32ServerIdentity const* expectedServer) {
    const auto connectionDeadline = std::min(apc::control::DeadlineAfter(waitMs), overallDeadline);
    const auto firstInstance = static_cast<std::size_t>(request.CorrelationId.Low % apc::control::c_pipeInstanceCount);
    bool sawRejectedEndpoint = false;
    bool sawDifferentServer = false;

    while (true) {
        for (std::size_t offset = 0; offset < apc::control::c_pipeInstanceCount; ++offset) {
            const auto instance = (firstInstance + offset) % apc::control::c_pipeInstanceCount;
            const auto instanceName = apc::control::PipeInstanceName(pipeName, instance);
            HANDLE pipe = CreateFileW(instanceName.c_str(),
                                      GENERIC_READ | FILE_WRITE_DATA,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                      nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) sawRejectedEndpoint = true;
                continue;
            }
            const auto closePipe = [pipe]() { CloseHandle(pipe); };
            struct Guard {
                decltype(closePipe)& Close;
                ~Guard() { Close(); }
            } guard{closePipe};

            auto currentServer = CaptureServerIdentity(pipe, expectedUnpackagedServerIdentity);
            if (!currentServer) {
                sawRejectedEndpoint = true;
                continue;
            }
            if (expectedServer && !IsSameLiveServer(*expectedServer, *currentServer)) {
                sawDifferentServer = true;
                continue;
            }
            observedServer = currentServer;

            const auto exchangeDeadline =
                std::min(apc::control::DeadlineAfter(c_pipeExchangeTimeoutMs), overallDeadline);
            if (apc::control::RemainingWait(exchangeDeadline) == 0) {
                return apc::control::client::AttemptResult::NotConnected;
            }
            if (apc::control::WriteRequest(pipe, request, nullptr, exchangeDeadline) !=
                apc::control::IoStatus::Success) {
                return apc::control::client::AttemptResult::Indeterminate;
            }
            if (apc::control::ReadResponse(pipe, response, nullptr, exchangeDeadline) !=
                    apc::control::IoStatus::Success ||
                response.CorrelationId != request.CorrelationId) {
                return apc::control::client::AttemptResult::Indeterminate;
            }
            (void)apc::control::WriteAcknowledgement(pipe, request.CorrelationId, nullptr, exchangeDeadline);
            return apc::control::client::AttemptResult::Complete;
        }

        if (apc::control::RemainingWait(connectionDeadline) == 0) {
            if (sawDifferentServer) return apc::control::client::AttemptResult::ServerChanged;
            return sawRejectedEndpoint ? apc::control::client::AttemptResult::Rejected
                                       : apc::control::client::AttemptResult::NotConnected;
        }
        Sleep(std::min<DWORD>(c_retryIntervalMs, apc::control::RemainingWait(connectionDeadline)));
    }
}

class Win32Transport final : public apc::control::client::Transport {
public:
    apc::control::client::AttemptResult
    TrySendOnce(apc::control::Request const& request,
                apc::control::Response& response,
                DWORD waitMs,
                std::uint64_t overallDeadline,
                apc::control::client::ServerIdentityPtr& observedServer,
                apc::control::client::ServerIdentityPtr const& expectedServer) override {
        if (!m_pipeName) return apc::control::client::AttemptResult::NotConnected;
        auto expected = std::dynamic_pointer_cast<Win32ServerIdentity const>(expectedServer);
        if (expectedServer && !expected) return apc::control::client::AttemptResult::ServerChanged;
        std::shared_ptr<Win32ServerIdentity> observed;
        const auto result = ::TrySendOnce(request,
                                          response,
                                          waitMs,
                                          overallDeadline,
                                          *m_pipeName,
                                          m_expectedUnpackagedServerIdentity,
                                          observed,
                                          expected.get());
        observedServer = std::move(observed);
        return result;
    }

    bool LaunchPackagedApp() override { return TryLaunchPackagedApp(); }

private:
    std::optional<std::wstring> m_pipeName = apc::control::PipeName();
    std::optional<apc::control::ExecutableFileIdentity> m_expectedUnpackagedServerIdentity = [] {
        auto path = ExpectedUnpackagedServerPath();
        return path ? apc::control::ExecutableIdentityFromPath(*path) : std::nullopt;
    }();
};

} // namespace

int Run(int argc, wchar_t** argv, bool jsonRequestedOnError, apc::control::ExitCode& unexpectedFailureCode) {
    auto parsed = ParseCommandLine(argc, argv);
    const bool jsonRequested =
        parsed.Send ? (parsed.Request.Flags & apc::control::CommandFlagJson) != 0 : jsonRequestedOnError;
    if (!parsed.Send) {
        if (parsed.ExitCode == 0) {
            WriteStdout(parsed.Message);
        } else {
            const auto code = static_cast<apc::control::ExitCode>(parsed.ExitCode);
            unexpectedFailureCode = code;
            WriteStderr(LocalError(jsonRequested, code, parsed.Message));
        }
        return static_cast<int>(parsed.ExitCode);
    }

    parsed.Request.CorrelationId = CreateCorrelationId();
    apc::control::Response response;
    Win32Transport transport;
    unexpectedFailureCode = apc::control::ExitCode::Indeterminate;
    const auto sendResult = apc::control::client::SendRequest(transport, parsed.Request, response);
    if (sendResult == apc::control::client::SendResult::InvalidRequest) {
        unexpectedFailureCode = apc::control::ExitCode::InvalidRequest;
        WriteStderr(
            LocalError(jsonRequested, unexpectedFailureCode, L"The command request is invalid and was not sent.\n"));
        return static_cast<int>(apc::control::ExitCode::InvalidRequest);
    }
    if (sendResult == apc::control::client::SendResult::Indeterminate) {
        WriteStderr(LocalError(
            jsonRequested,
            unexpectedFailureCode,
            L"The command may have completed, but the result could not be confirmed. It was not sent to a different "
            L"app instance.\n"));
        return static_cast<int>(apc::control::ExitCode::Indeterminate);
    }
    if (sendResult != apc::control::client::SendResult::Complete) {
        unexpectedFailureCode = apc::control::ExitCode::Unavailable;
        WriteStderr(LocalError(jsonRequested,
                               unexpectedFailureCode,
                               L"AudioPlaybackConnector2 is not running or did not accept the command.\n"));
        return static_cast<int>(apc::control::ExitCode::Unavailable);
    }

    unexpectedFailureCode = response.Code;
    if (response.Payload.empty() && response.Code != apc::control::ExitCode::Success) {
        WriteStderr(
            LocalError(jsonRequested, response.Code, L"The command result did not include a diagnostic message.\n"));
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
        jsonRequested = JsonRequested(argc, argv);
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
