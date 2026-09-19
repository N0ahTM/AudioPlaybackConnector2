#include "TestCheck.hpp"
#include <control/CliParser.hpp>

#include <initializer_list>
#include <string>
#include <vector>

namespace {
using namespace apc::control;

constexpr wchar_t c_expectedHelp[] = LR"(AudioPlaybackConnector2 command line control

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

cli::ParseResult Parse(std::initializer_list<wchar_t const*> args) {
    std::vector<wchar_t const*> argv{L"apc2ctl"};
    argv.insert(argv.end(), args);
    return cli::ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

void TestCommandRequests() {
    struct Case {
        std::initializer_list<wchar_t const*> Args;
        CommandType Command;
        TargetKind Target = TargetKind::None;
        std::wstring Payload;
        std::uint32_t Flags = 0;
    };
    const Case cases[]{
        {{L"show"}, CommandType::Show},
        {{L"settings"}, CommandType::Settings},
        {{L"status", L"--json"}, CommandType::Status, TargetKind::None, L"", CommandFlagJson},
        {{L"LIST", L"--RAW", L"--json", L"--json"}, CommandType::List, TargetKind::None, L"", 3},
        {{L"disconnect-all"}, CommandType::DisconnectAll},
        {{L"reconnect-all"}, CommandType::ReconnectAll},
        {{L"connect", L"--id", L"device"}, CommandType::Connect, TargetKind::Id, L"device"},
        {{L"disconnect", L"--last"}, CommandType::Disconnect, TargetKind::Last},
        {{L"reconnect", L"--default"}, CommandType::Reconnect, TargetKind::Default},
        {{L"toggle"}, CommandType::ToggleLast, TargetKind::Default},
        {{L"connect", L"--name", L"Kopfh\u00f6rer \u84dd\u7259"},
         CommandType::Connect,
         TargetKind::Name,
         L"Kopfh\u00f6rer \u84dd\u7259"},
        {{L"connect", L"--mac", L"AA:BB"}, CommandType::Connect, TargetKind::Mac, L"AA:BB"},
        {{L"connect", L"--alias", L"Desk"}, CommandType::Connect, TargetKind::Alias, L"Desk"},
        {{L"connect", L"--", L"--json"}, CommandType::Connect, TargetKind::Auto, L"--json"},
        {{L"connect", L"--id", L"--", L"-id", L"--json"},
         CommandType::Connect,
         TargetKind::Id,
         L"-id",
         CommandFlagJson},
        {{L"default", L"show", L"--raw"}, CommandType::DefaultShow, TargetKind::None, L"", CommandFlagRaw},
        {{L"default", L"clear"}, CommandType::DefaultClear},
        {{L"default", L"set", L"Speaker"}, CommandType::DefaultSet, TargetKind::Auto, L"Speaker"},
        {{L"alias", L"list"}, CommandType::AliasList},
        {{L"alias", L"clear", L"--id", L"device"}, CommandType::AliasClear, TargetKind::Id, L"device"},
        {{L"alias", L"set", L"device", L"Desk"}, CommandType::AliasSet, TargetKind::Auto, L"device\nDesk"},
        {{L"alias", L"set", L"--value", L"Desk", L"--id", L"device"},
         CommandType::AliasSet,
         TargetKind::Id,
         L"device\nDesk"},
        {{L"alias", L"set", L"--id", L"device", L"--value", L"--", L"-Desk"},
         CommandType::AliasSet,
         TargetKind::Id,
         L"device\n-Desk"},
    };
    for (auto const& test : cases) {
        auto const result = Parse(test.Args);
        Check(result.Send && result.ExitCode == 0 && result.Message.empty() && result.Request.Command == test.Command &&
                  result.Request.Target == test.Target && result.Request.Payload == test.Payload &&
                  result.Request.Flags == test.Flags && result.Request.CorrelationId.Empty(),
              "CLI parsing must preserve the exact command, selector, UTF-16 payload and flags without activating "
              "transport");
    }
}

void TestParseErrors() {
    struct Case {
        std::initializer_list<wchar_t const*> Args;
        wchar_t const* Message;
    };
    const Case cases[]{
        {{L"connect"}, L"A device target is required.\n"},
        {{L"connect", L""}, L"A non-empty device target is required.\n"},
        {{L"connect", L"--id"}, L"Missing value for --id.\n"},
        {{L"connect", L"--NAME", L"--json"}, L"Missing value for --NAME.\n"},
        {{L"connect", L"--id", L""}, L"Missing value for --id.\n"},
        {{L"connect", L"--id", L"--"}, L"Missing value for --id.\n"},
        {{L"connect", L"--id", L"a", L"--id", L"b"}, L"Only one device target selector is supported.\n"},
        {{L"connect", L"--id", L"a", L"b"}, L"A positional target cannot be combined with a target selector.\n"},
        {{L"connect", L"a", L"b"}, L"Only one positional target is supported.\n"},
        {{L"connect", L"--bogus"}, L"Unknown option: --bogus\n"},
        {{L"default"}, L"default requires show, set, or clear.\n"},
        {{L"default", L"bogus"}, L"Unknown default command: bogus\n"},
        {{L"default", L"set", L"--last"}, L"This command requires an explicit device target.\n"},
        {{L"alias"}, L"alias requires list, set, or clear.\n"},
        {{L"alias", L"bogus"}, L"Unknown alias command: bogus\n"},
        {{L"alias", L"set", L"device"}, L"A non-empty alias value is required.\n"},
        {{L"alias", L"set", L"--value", L""}, L"Missing value for --value.\n"},
        {{L"alias", L"set", L"a", L"b", L"c"}, L"Only one alias value is supported.\n"},
        {{L"status", L"--"}, L"Unknown option: --\n"},
    };
    for (auto const& test : cases) {
        auto const result = Parse(test.Args);
        Check(!result.Send && result.ExitCode == 3 && result.Message == test.Message,
              "CLI invalid input must preserve exact diagnostic text and exit code");
    }
    auto const help = Parse({});
    Check(!help.Send && help.ExitCode == 0 && help.Message == c_expectedHelp,
          "no arguments must display help without sending a request");
    Check(Parse({L"HELP", L"ignored"}).Message == help.Message && Parse({L"--help"}).Message == help.Message &&
              Parse({L"-h"}).Message == help.Message &&
              Parse({L"bogus"}).Message == L"Unknown command: bogus\n\n" + help.Message,
          "help aliases and unknown-command output must retain the same help text");
}

void TestLocalErrorOutput() {
    Check(cli::LocalError(false, ExitCode::InvalidRequest, L"Error\r\n") == L"Error\r\n",
          "text errors retain their original line endings");
    Check(cli::LocalError(true, ExitCode::InvalidRequest, L"A\"\\\t\n\r\n") ==
              L"{\"ok\":false,\"exitCode\":3,\"message\":\"A\\\"\\\\\\t\"}\n",
          "JSON errors escape content, remove trailing line endings and terminate exactly once");
    wchar_t const* escaped[]{L"apc2ctl", L"connect", L"--id", L"--", L"--json"};
    wchar_t const* requested[]{L"apc2ctl", L"connect", L"--id", L"--JSON"};
    Check(!cli::JsonRequested(5, escaped) && cli::JsonRequested(4, requested),
          "error output detection must distinguish escaped values from actual JSON options");
}
} // namespace

int RunCliParserTests() {
    TestCommandRequests();
    TestParseErrors();
    TestLocalErrorOutput();
    return g_failures;
}
