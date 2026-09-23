#include <app/RatingPromptPolicy.hpp>

#include <windows.h>

#include <format>

namespace apc::app {
namespace {

constexpr int c_minDaysSinceFirstLaunch = 14;
constexpr int c_minUsageDays = 3;

// Howard Hinnant's civil-date conversion, public domain.
std::optional<std::int64_t> DaysFromIsoDate(std::wstring_view isoDate) noexcept {
    if (isoDate.size() != 10 || isoDate[4] != L'-' || isoDate[7] != L'-') return std::nullopt;
    try {
        const auto year = std::stoi(std::wstring(isoDate.substr(0, 4)));
        const auto month = std::stoi(std::wstring(isoDate.substr(5, 2)));
        const auto day = std::stoi(std::wstring(isoDate.substr(8, 2)));
        if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
        const std::int64_t adjustedYear = year - (month <= 2);
        const std::int64_t era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
        const auto yearOfEra = adjustedYear - era * 400;
        const auto monthPrime = month + (month > 2 ? -3 : 9);
        const std::int64_t dayOfYear = (153 * monthPrime + 2) / 5 + day - 1;
        return era * 146097 + yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear - 719468;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::wstring TodayLocalIsoDate() {
    try {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        return std::format(L"{:04}-{:02}-{:02}", local.wYear, local.wMonth, local.wDay);
    } catch (...) {
        return {};
    }
}

bool IsRatingPromptEligible(bool isStoreChannel, RatingPromptData const& data, std::wstring_view today) noexcept {
    if (!isStoreChannel || data.Asked) return false;
    const auto todaySerial = DaysFromIsoDate(today);
    const auto firstLaunch = DaysFromIsoDate(data.FirstLaunchDate);
    if (!todaySerial || !firstLaunch) return false;
    if (*todaySerial - *firstLaunch < c_minDaysSinceFirstLaunch) return false;
    return data.UsageDays >= c_minUsageDays;
}

} // namespace apc::app
