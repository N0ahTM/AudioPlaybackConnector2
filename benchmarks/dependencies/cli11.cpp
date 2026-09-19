#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

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
}
