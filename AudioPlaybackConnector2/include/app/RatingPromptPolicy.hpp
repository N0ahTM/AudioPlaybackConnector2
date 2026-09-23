#pragma once

#include <core/SettingsData.hpp>

#include <optional>
#include <string>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Rating Prompt Policy //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace apc::app {

// The store rating notification is eligible only in the Store channel, at least 14
// days after first launch, after at least three days with a successful device
// connection, and at most once per installation.
[[nodiscard]] bool
IsRatingPromptEligible(bool isStoreChannel, RatingPromptData const& data, std::wstring_view today) noexcept;

// Local calendar day as ISO YYYY-MM-DD. Empty when formatting fails.
[[nodiscard]] std::wstring TodayLocalIsoDate();

} // namespace apc::app
