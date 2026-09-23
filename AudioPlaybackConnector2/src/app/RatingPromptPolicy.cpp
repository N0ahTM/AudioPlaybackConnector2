#include <app/RatingPromptPolicy.hpp>

#include <windows.h>

#include <chrono>
#include <format>

namespace apc::app {
namespace {

constexpr int c_minDaysSinceFirstLaunch = 14;
constexpr int c_minUsageDays = 3;

std::optional<std::chrono::sys_days> DayFromIsoDate(std::wstring_view isoDate) noexcept {
    if (isoDate.size() != 10 || isoDate[4] != L'-' || isoDate[7] != L'-') return std::nullopt;
    for (std::size_t i = 0; i < isoDate.size(); ++i) {
        if (i != 4 && i != 7 && (isoDate[i] < L'0' || isoDate[i] > L'9')) return std::nullopt;
    }
    const auto digit = [&](std::size_t i) { return isoDate[i] - L'0'; };
    const int year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
    const unsigned month = digit(5) * 10 + digit(6);
    const unsigned day = digit(8) * 10 + digit(9);
    const auto date = std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};
    if (!date.ok()) return std::nullopt;
    return std::chrono::sys_days{date};
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
    const auto todayDay = DayFromIsoDate(today);
    const auto firstLaunch = DayFromIsoDate(data.FirstLaunchDate);
    if (!todayDay || !firstLaunch) return false;
    if (*todayDay - *firstLaunch < std::chrono::days{c_minDaysSinceFirstLaunch}) return false;
    return data.UsageDays >= c_minUsageDays;
}

} // namespace apc::app
