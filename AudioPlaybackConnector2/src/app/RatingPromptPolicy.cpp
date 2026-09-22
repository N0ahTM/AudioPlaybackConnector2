#include <app/RatingPromptPolicy.hpp>

#include <windows.h>

#include <format>

namespace apc::app {
namespace {

constexpr int c_minDaysSinceFirstLaunch = 14;
constexpr int c_minUsageDays = 3;
constexpr int c_deferDays = 14;
constexpr int c_maxDeferCount = 1;

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

std::wstring IsoDateFromDays(std::int64_t days) {
    const std::int64_t civil = days + 719468;
    const std::int64_t era = (civil >= 0 ? civil : civil - 146096) / 146097;
    const auto dayOfEra = civil - era * 146097;
    const auto yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t year = yearOfEra + era * 400;
    const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthPrime = (5 * dayOfYear + 2) / 153;
    const auto day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const auto month = monthPrime + (monthPrime < 10 ? 3 : -9);
    return std::format(L"{:04}-{:02}-{:02}", year + (month <= 2), month, day);
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

std::wstring AddDaysToIsoDate(std::wstring_view isoDate, int days) {
    const auto serial = DaysFromIsoDate(isoDate);
    if (!serial) return {};
    return IsoDateFromDays(*serial + days);
}

bool IsRatingPromptEligible(bool isStoreChannel, RatingPromptData const& data, std::wstring_view today) noexcept {
    if (!isStoreChannel) return false;
    if (data.State == RatingPromptState::Completed || data.State == RatingPromptState::Disabled) return false;
    const auto todaySerial = DaysFromIsoDate(today);
    const auto firstLaunch = DaysFromIsoDate(data.FirstLaunchDate);
    if (!todaySerial || !firstLaunch) return false;
    if (*todaySerial - *firstLaunch < c_minDaysSinceFirstLaunch) return false;
    if (data.UsageDays < c_minUsageDays) return false;
    if (data.State == RatingPromptState::Deferred) {
        const auto deferUntil = DaysFromIsoDate(data.DeferUntil);
        if (!deferUntil || *todaySerial < *deferUntil) return false;
    }
    return true;
}

RatingPromptData ApplyRatingPromptOutcome(RatingPromptOutcome outcome, RatingPromptData data, std::wstring_view today) {
    switch (outcome) {
        case RatingPromptOutcome::Rated: data.State = RatingPromptState::Completed; break;
        case RatingPromptOutcome::Dismissed: data.State = RatingPromptState::Disabled; break;
        case RatingPromptOutcome::Deferred:
            if (data.DeferCount >= c_maxDeferCount) {
                data.State = RatingPromptState::Disabled;
            } else {
                data.State = RatingPromptState::Deferred;
                data.DeferUntil = AddDaysToIsoDate(today, c_deferDays);
                ++data.DeferCount;
            }
            break;
    }
    return data;
}

} // namespace apc::app
