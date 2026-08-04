#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace apc::limits {

inline constexpr std::size_t c_maxSettingsFileBytes = 4 * 1024 * 1024;
inline constexpr std::size_t c_maxPersistedDeviceCount = 384;
inline constexpr std::size_t c_maxDeviceIdCharacters = 512;
inline constexpr std::size_t c_maxDeviceNameCharacters = 256;
inline constexpr std::size_t c_maxDeviceAliasCharacters = 128;
inline constexpr std::size_t c_maxLanguageCharacters = 64;
inline constexpr std::size_t c_maxVersionCharacters = 128;
inline constexpr std::uint32_t c_minWindowDpi = 48;
inline constexpr std::uint32_t c_maxWindowDpi = 960;

// Six bytes covers the longest JSON escape for one UTF-16 code unit; both device collections may contain every id.
inline constexpr std::size_t c_maxJsonBytesPerUtf16CodeUnit = 6;
inline constexpr std::size_t c_maxSerializedDeviceBudget =
    ((c_maxDeviceIdCharacters * 2) + c_maxDeviceNameCharacters + c_maxDeviceAliasCharacters) *
        c_maxJsonBytesPerUtf16CodeUnit +
    512;
inline constexpr std::size_t c_settingsFixedSerializationBudget = 128 * 1024;
static_assert(c_maxPersistedDeviceCount * c_maxSerializedDeviceBudget + c_settingsFixedSerializationBudget <=
              c_maxSettingsFileBytes);

[[nodiscard]] inline bool IsValidUtf16(std::wstring_view value) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<std::uint16_t>(value[index]);
        if (character >= 0xD800 && character <= 0xDBFF) {
            if (++index >= value.size()) return false;
            const auto low = static_cast<std::uint16_t>(value[index]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
        } else if (character >= 0xDC00 && character <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool IsBoundedUtf16(std::wstring_view value, std::size_t limit) noexcept {
    return value.size() <= limit && !value.contains(L'\0') && IsValidUtf16(value);
}

[[nodiscard]] inline bool IsSupportedLanguage(std::wstring_view value) noexcept {
    return value == L"system" || value == L"en" || value == L"de" || value == L"fr" || value == L"es" ||
           value == L"ja" || value == L"ko" || value == L"zh_hans" || value == L"zh_hant";
}

[[nodiscard]] inline std::wstring TruncateUtf16(std::wstring_view value, std::size_t limit) {
    if (value.contains(L'\0') || !IsValidUtf16(value)) return {};
    auto length = std::min(value.size(), limit);
    if (length != value.size() && length != 0) {
        const auto last = static_cast<std::uint16_t>(value[length - 1]);
        if (last >= 0xD800 && last <= 0xDBFF) --length;
    }
    return std::wstring(value.substr(0, length));
}

} // namespace apc::limits
