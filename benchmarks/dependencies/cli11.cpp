#include <CLI/CLI.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Parser Capability Probe ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int main() {
    CLI::App parser{"APC dependency probe"};
    std::string name;
    std::string id;
    auto* connect = parser.add_subcommand("connect");
    auto* nameOption = connect->add_option("--name", name);
    auto* idOption = connect->add_option("--id", id);
    nameOption->excludes(idOption);
    connect->require_option(1, 1);
    parser.require_subcommand(1, 1);
    char const* valid[]{"probe", "connect", "--name", "Kopfhörer 蓝牙"};
    parser.parse(4, valid);
    if (name != "Kopfhörer 蓝牙") return 1;
    char const* conflict[]{"probe", "connect", "--name", "headset", "--id", "device"};
    bool rejected = false;
    try {
        parser.parse(6, conflict);
    } catch (CLI::ParseError const&) {
        rejected = true;
    }
    if (!rejected) return 2;
    std::cout << "unicode and exclusive-option probes passed\n";

    struct ContractCase {
        char const* Name;
        std::vector<std::string> Arguments;
        bool ProductAccepts;
        std::string ProductId;
    };
    const ContractCase contracts[]{
        {"plain-id", {"connect", "--id", "device"}, true, "device"},
        {"escaped-option-value", {"connect", "--id", "--", "-device"}, true, "-device"},
        {"equals-assignment", {"connect", "--id=device"}, false, {}},
        {"unescaped-leading-dash", {"connect", "--id", "-device"}, false, {}},
        {"empty-id", {"connect", "--id", ""}, false, {}},
        {"duplicate-selector", {"connect", "--id", "a", "--id", "b"}, false, {}},
        {"missing-id", {"connect", "--id"}, false, {}},
    };
    std::size_t differences = 0;
    for (auto const& test : contracts) {
        CLI::App command{"APC contract probe"};
        std::string parsedId;
        auto* connectCommand = command.add_subcommand("connect");
        connectCommand->add_option("--id", parsedId)->required();
        command.require_subcommand(1, 1);
        auto arguments = test.Arguments;
        std::reverse(arguments.begin(), arguments.end());
        bool accepted = true;
        std::string diagnostic;
        int exitCode = 0;
        try {
            command.parse(arguments);
        } catch (CLI::ParseError const& error) {
            accepted = false;
            diagnostic = error.what();
            exitCode = error.get_exit_code();
        }
        const bool agrees = accepted == test.ProductAccepts && (!accepted || parsedId == test.ProductId);
        if (!agrees) ++differences;
        std::cout << test.Name << ": agrees=" << agrees << " accepted=" << accepted << " exit=" << exitCode
                  << " diagnostic=" << diagnostic << '\n';
    }
    // This records adaptation work, not adoption approval. Product error text and
    // exit codes also have to pass their separate golden tests before replacement.
    std::cout << "native CLI11 acceptance differences=" << differences << '\n';
}
