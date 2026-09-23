#include "TestCheck.hpp"

#include <core/SettingsCodec.hpp>

#include <stdexcept>
#include <string_view>

namespace {
void TestCurrentFormat() {
    Check(apc::settings::Decode(R"({"schemaVersion":2})") == SettingsData{},
          "missing optional current-format fields must use current defaults");

    SettingsData unicode;
    unicode.Devices.push_back({L"device", L"Kopfh\u00f6rer \u97f3", L"Desk", true, false});
    Check(apc::settings::Decode(apc::settings::Encode(unicode)) == unicode,
          "UTF-8 persistence must round-trip Unicode device names");

    SettingsData prompted;
    prompted.RatingPrompt.FirstLaunchDate = L"2026-09-08";
    prompted.RatingPrompt.UsageDays = 4;
    prompted.RatingPrompt.LastUsageDate = L"2026-09-20";
    prompted.RatingPrompt.Asked = true;
    Check(apc::settings::Decode(apc::settings::Encode(prompted)) == prompted,
          "the rating prompt state must round-trip through the current format");
    const auto expectRejected = [](std::string_view bytes, std::string_view message) {
        try {
            static_cast<void>(apc::settings::Decode(bytes));
            Check(false, message);
        } catch (std::invalid_argument const&) {
            Check(true, message);
        }
    };
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"firstLaunchDate":"2026-99-99"}})",
                   "malformed rating prompt dates must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"firstLaunchDate":"2026-02-31"}})",
                   "impossible rating prompt calendar days must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"usageDays":-3}})",
                   "negative rating prompt counters must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"unexpected":1}})",
                   "unknown rating prompt keys must be rejected");
}
} // namespace

int RunSettingsCodecTests() {
    TestCurrentFormat();
    return g_failures;
}
