#include <control/CliParser.hpp>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <utility>

namespace apc::control::cli {
namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Local Parsing Rules ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::wstring_view ToView(wchar_t const* value) {
    return value ? std::wstring_view(value) : std::wstring_view();
}

bool EqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](wchar_t a, wchar_t b) {
               return towlower(a) == towlower(b);
           });
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

std::optional<std::wstring> ReadOptionValue(int& index, int argc, wchar_t const* const* argv) {
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

ParseResult
ParseTargetOptions(apc::control::CommandType command, int startIndex, int argc, wchar_t const* const* argv) {
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

ParseResult ParseDefaultCommand(int startIndex, int argc, wchar_t const* const* argv) {
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

ParseResult ParseAliasSetOptions(int startIndex, int argc, wchar_t const* const* argv) {
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

ParseResult ParseAliasCommand(int startIndex, int argc, wchar_t const* const* argv) {
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

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Command Line and Error Output /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool JsonRequested(int argc, wchar_t const* const* argv) noexcept {
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

ParseResult ParseCommandLine(int argc, wchar_t const* const* argv) {
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

} // namespace apc::control::cli
