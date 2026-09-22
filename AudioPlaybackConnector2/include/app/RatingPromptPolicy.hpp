#pragma once

#include <core/SettingsData.hpp>

#include <optional>
#include <string>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Rating Prompt Policy //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace apc::app {

enum class RatingPromptOutcome { Rated, Deferred, Dismissed };

// The store rating prompt is eligible only in the Store channel, at least 14 days
// after first launch and after at least three days with a successful device connection,
// and only when the current state permits asking again.
[[nodiscard]] bool
IsRatingPromptEligible(bool isStoreChannel, RatingPromptData const& data, std::wstring_view today) noexcept;

// Rated is terminal (Completed), Dismissed is terminal (Disabled); Deferred moves the
// prompt 14 days out exactly once, a second deferral disables it permanently.
[[nodiscard]] RatingPromptData
ApplyRatingPromptOutcome(RatingPromptOutcome outcome, RatingPromptData data, std::wstring_view today);

// Local calendar day as ISO YYYY-MM-DD. Empty when parsing/formatting fails.
[[nodiscard]] std::wstring TodayLocalIsoDate();
[[nodiscard]] std::wstring AddDaysToIsoDate(std::wstring_view isoDate, int days);

} // namespace apc::app
