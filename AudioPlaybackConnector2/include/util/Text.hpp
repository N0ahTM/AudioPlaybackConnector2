#pragma once

#include <cwctype>
#include <string>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Case and Hex Text Helpers //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

inline std::wstring LowerInvariant(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (wchar_t character : value) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return lowered;
}

inline bool EqualsIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
    return lhs.size() == rhs.size() && LowerInvariant(lhs) == LowerInvariant(rhs);
}

inline bool ContainsIgnoreCase(std::wstring_view value, std::wstring_view query) {
    return !query.empty() && LowerInvariant(value).find(LowerInvariant(query)) != std::wstring::npos;
}

inline std::wstring NormalizeHex(std::wstring_view value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t character : value) {
        if ((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F')) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return normalized;
}

} // namespace util
