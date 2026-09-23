#pragma once

#include <core/SettingsData.hpp>

#include <string>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Rating Prompt Policy //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace apc::app {

// After package feature admission, require 14 days since first launch, three
// days with a successful connection, and at most one prompt per installation.
[[nodiscard]] bool IsRatingPromptEligible(RatingPromptData const& data, std::wstring_view today) noexcept;

// Local calendar day as ISO YYYY-MM-DD. Empty when formatting fails.
[[nodiscard]] std::wstring TodayLocalIsoDate();

} // namespace apc::app
