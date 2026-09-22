#include <control/CliParser.hpp>
#include <util/Text.hpp>
#include <CLI/CLI.hpp>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <utility>
#include <iterator>
#include <vector>

namespace apc::control::cli {
namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Local Parsing Rules ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::wstring_view ToView(wchar_t const* value) {
    return value ? std::wstring_view(value) : std::wstring_view();
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
    return {.Send = false, .Request = {}, .ExitCode = exitCode, .Message = std::move(message)};
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

struct ParseFailure {
    std::wstring Message;
};
struct OptionInput {
    std::wstring_view Name;
    std::optional<std::wstring> Value;
};
struct Selector {
    std::wstring_view Name;
    TargetKind Kind;
};
constexpr Selector c_selectors[]{{L"--id", TargetKind::Id},
                                 {L"--name", TargetKind::Name},
                                 {L"--mac", TargetKind::Mac},
                                 {L"--alias", TargetKind::Alias}};
struct CommandSpec {
    std::wstring_view Name;
    std::wstring_view Subcommand;
    CommandType Kind;
};
constexpr CommandSpec c_commands[]{
    {L"show", L"", CommandType::Show},
    {L"settings", L"", CommandType::Settings},
    {L"status", L"", CommandType::Status},
    {L"list", L"", CommandType::List},
    {L"disconnect-all", L"", CommandType::DisconnectAll},
    {L"reconnect-all", L"", CommandType::ReconnectAll},
    {L"connect", L"", CommandType::Connect},
    {L"disconnect", L"", CommandType::Disconnect},
    {L"reconnect", L"", CommandType::Reconnect},
    {L"toggle", L"", CommandType::ToggleLast},
    {L"default", L"show", CommandType::DefaultShow},
    {L"default", L"clear", CommandType::DefaultClear},
    {L"default", L"set", CommandType::DefaultSet},
    {L"alias", L"list", CommandType::AliasList},
    {L"alias", L"clear", CommandType::AliasClear},
    {L"alias", L"set", CommandType::AliasSet},
};

ParseResult ParseWithCli(CommandSpec const& spec, int argc, wchar_t const* const* argv) {
    const bool relative = spec.Kind == CommandType::Connect || spec.Kind == CommandType::Disconnect ||
                          spec.Kind == CommandType::Reconnect || spec.Kind == CommandType::ToggleLast;
    const bool aliasValue = spec.Kind == CommandType::AliasSet;
    const bool targets =
        relative || aliasValue || spec.Kind == CommandType::DefaultSet || spec.Kind == CommandType::AliasClear;
    auto narrowName = [](std::wstring_view name) {
        std::string text;
        for (auto character : name)
            text.push_back(static_cast<char>(character));
        return text;
    };
    std::vector<OptionInput> inputs;
    std::vector<std::string> tokens{narrowName(spec.Name)};
    if (!spec.Subcommand.empty()) tokens.push_back(narrowName(spec.Subcommand));
    bool ended = false;
    // CLI11 sees generated indexes for values, preserving opaque UTF-16 and the
    // public escape grammar without allowing values to become parser syntax.
    for (int i = spec.Subcommand.empty() ? 2 : 3; i < argc; ++i) {
        const auto arg = ToView(argv[i]);
        if (targets && !ended && arg == L"--") {
            ended = true;
            continue;
        }
        const auto selector = std::ranges::find_if(
            c_selectors, [&](auto const& value) { return util::EqualsIgnoreCase(arg, value.Name); });
        if (!ended && (util::EqualsIgnoreCase(arg, L"--json") || util::EqualsIgnoreCase(arg, L"--raw"))) {
            tokens.push_back(util::EqualsIgnoreCase(arg, L"--json") ? "--json" : "--raw");
            continue;
        }
        if (!ended && targets && !aliasValue &&
            (util::EqualsIgnoreCase(arg, L"--last") || util::EqualsIgnoreCase(arg, L"--default"))) {
            tokens.push_back(util::EqualsIgnoreCase(arg, L"--last") ? "--last" : "--default");
            continue;
        }
        const auto index = "v" + std::to_string(inputs.size());
        if (!ended && targets &&
            (selector != std::end(c_selectors) || (aliasValue && util::EqualsIgnoreCase(arg, L"--value")))) {
            inputs.push_back({arg, ReadOptionValue(i, argc, argv)});
            tokens.push_back((selector != std::end(c_selectors) ? narrowName(selector->Name) : "--value") + "=" +
                             index);
        } else {
            inputs.push_back({arg, std::wstring(arg)});
            tokens.push_back(!targets || (!ended && !arg.empty() && arg.front() == L'-') ? "--invalid=" + index
                                                                                         : index);
        }
    }
    Request request;
    request.Command = spec.Kind;
    std::optional<std::wstring> positional, alias;
    auto fail = [](std::wstring message) { throw ParseFailure{std::move(message)}; };
    auto input = [&](std::string const& index) -> OptionInput const& { return inputs.at(std::stoul(index.substr(1))); };
    auto select = [&](TargetKind kind) {
        if (request.Target != TargetKind::None || positional) fail(L"Only one device target selector is supported.\n");
        request.Target = kind;
    };
    CLI::App parser;
    parser.set_help_flag();
    auto* command = parser.add_subcommand(narrowName(spec.Name));
    if (!spec.Subcommand.empty()) command = command->add_subcommand(narrowName(spec.Subcommand));
    command->add_flag_callback("--json", [&] { request.Flags |= CommandFlagJson; })->trigger_on_parse();
    command->add_flag_callback("--raw", [&] { request.Flags |= CommandFlagRaw; })->trigger_on_parse();
    command
        ->add_option_function<std::string>(
            "--invalid",
            [&](auto const& value) { fail(L"Unknown option: " + std::wstring(input(value).Name) + L"\n"); })
        ->trigger_on_parse();
    for (auto const& selector : c_selectors) {
        command
            ->add_option_function<std::string>(narrowName(selector.Name),
                                               [&, kind = selector.Kind](auto const& value) {
                                                   select(kind);
                                                   auto const& option = input(value);
                                                   if (!option.Value || option.Value->empty())
                                                       fail(L"Missing value for " + std::wstring(option.Name) + L".\n");
                                                   request.Payload = *option.Value;
                                               })
            ->trigger_on_parse();
    }
    for (auto const& choice : {Selector{L"--last", TargetKind::Last}, Selector{L"--default", TargetKind::Default}}) {
        command
            ->add_flag_callback(narrowName(choice.Name),
                                [&, kind = choice.Kind] {
                                    if (!relative) fail(L"This command requires an explicit device target.\n");
                                    select(kind);
                                })
            ->trigger_on_parse();
    }
    command
        ->add_option_function<std::string>("--value",
                                           [&](auto const& value) {
                                               auto const& option = input(value);
                                               if (!option.Value || option.Value->empty())
                                                   fail(L"Missing value for --value.\n");
                                               if (alias) fail(L"Only one alias value is supported.\n");
                                               alias = *option.Value;
                                           })
        ->trigger_on_parse();
    command
        ->add_option_function<std::string>(
            "TARGET",
            [&](auto const& value) {
                if (request.Target == TargetKind::None && !positional)
                    positional = *input(value).Value;
                else if (aliasValue) {
                    if (alias) fail(L"Only one alias value is supported.\n");
                    alias = *input(value).Value;
                } else
                    fail(request.Target != TargetKind::None
                             ? L"A positional target cannot be combined with a target selector.\n"
                             : L"Only one positional target is supported.\n");
            })
        ->allow_extra_args()
        ->trigger_on_parse();
    std::reverse(tokens.begin(), tokens.end());
    parser.parse(tokens);
    if (positional) {
        request.Target = TargetKind::Auto;
        request.Payload = std::move(*positional);
    }
    if (targets && request.Target == TargetKind::None) {
        if (spec.Kind == CommandType::ToggleLast)
            request.Target = TargetKind::Default;
        else
            fail(L"A device target is required.\n");
    }
    if (targets && request.Target != TargetKind::Last && request.Target != TargetKind::Default &&
        request.Payload.empty())
        fail(aliasValue ? L"A device target is required.\n" : L"A non-empty device target is required.\n");
    if (aliasValue) {
        if (!alias || alias->empty()) fail(L"A non-empty alias value is required.\n");
        request.Payload += L"\n" + *alias;
    }
    return {.Send = true, .Request = std::move(request), .ExitCode = 0, .Message = {}};
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Command Line and Error Output /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool JsonRequested(int argc, wchar_t const* const* argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        const auto argument = ToView(argv[i]);
        if (util::EqualsIgnoreCase(argument, L"--id") || util::EqualsIgnoreCase(argument, L"--name") ||
            util::EqualsIgnoreCase(argument, L"--mac") || util::EqualsIgnoreCase(argument, L"--alias") ||
            util::EqualsIgnoreCase(argument, L"--value")) {
            if (i + 1 < argc && ToView(argv[i + 1]) == L"--") {
                i += std::min(2, argc - i - 1);
            } else if (i + 1 < argc && (ToView(argv[i + 1]).empty() || ToView(argv[i + 1]).front() != L'-')) {
                ++i;
            }
            continue;
        }
        if (argument == L"--") return false;
        if (util::EqualsIgnoreCase(argument, L"--json")) return true;
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
    if (argc <= 1) return Error(0, HelpText());
    auto command = ToView(argv[1]);
    if (util::EqualsIgnoreCase(command, L"help") || util::EqualsIgnoreCase(command, L"--help") ||
        util::EqualsIgnoreCase(command, L"-h"))
        return Error(0, HelpText());
    const bool family = util::EqualsIgnoreCase(command, L"default") || util::EqualsIgnoreCase(command, L"alias");
    if (family && argc < 3)
        return Error(3,
                     util::EqualsIgnoreCase(command, L"default") ? L"default requires show, set, or clear.\n"
                                                                 : L"alias requires list, set, or clear.\n");
    const auto subcommand = family ? ToView(argv[2]) : std::wstring_view{};
    const auto spec = std::ranges::find_if(c_commands, [&](auto const& value) {
        return util::EqualsIgnoreCase(command, value.Name) && util::EqualsIgnoreCase(subcommand, value.Subcommand);
    });
    if (spec == std::end(c_commands)) {
        if (family)
            return Error(3,
                         std::wstring(util::EqualsIgnoreCase(command, L"default") ? L"Unknown default command: "
                                                                                  : L"Unknown alias command: ") +
                             std::wstring(subcommand) + L"\n");
        return Error(3, L"Unknown command: " + std::wstring(command) + L"\n\n" + HelpText());
    }
    try {
        return ParseWithCli(*spec, argc, argv);
    } catch (ParseFailure const& failure) {
        return Error(3, failure.Message);
    }
}

} // namespace apc::control::cli
