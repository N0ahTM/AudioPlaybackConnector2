#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// JSON Capability Probe /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int main() {
    auto document = nlohmann::json::parse(R"({"schemaVersion":2,"alias":"Kopfhörer 蓝牙","enabled":true})");
    if (!document["schemaVersion"].is_number_integer() || document["schemaVersion"] != 2 ||
        document["alias"] != "Kopfhörer 蓝牙" || !document["enabled"].is_boolean())
        return 1;
    if (nlohmann::json::parse(document.dump()) != document) return 2;
    if (!nlohmann::json::parse("{broken", nullptr, false).is_discarded()) return 3;
    auto wrongType = nlohmann::json::parse(R"({"schemaVersion":"2"})");
    if (wrongType["schemaVersion"].is_number_integer()) return 4;
    std::cout << "unicode, roundtrip, syntax and type probes passed\n";
}
