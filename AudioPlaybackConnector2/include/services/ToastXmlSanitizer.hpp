#pragma once

#include <string>
#include <string_view>

namespace apc::toast {

inline std::wstring EscapeXml(std::wstring_view value) {
    constexpr wchar_t c_replacementCharacter = L'\uFFFD';
    std::wstring result;
    result.reserve(value.size());

    std::size_t index = 0;
    while (index < value.size()) {
        const auto ch = value[index];
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (index + 1 < value.size() && value[index + 1] >= 0xDC00 && value[index + 1] <= 0xDFFF) {
                result += ch;
                result += value[index + 1];
                index += 2;
            } else {
                result += c_replacementCharacter;
                ++index;
            }
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            result += c_replacementCharacter;
            ++index;
            continue;
        }
        if (ch != L'\t' && ch != L'\n' && ch != L'\r' && (ch < 0x20 || ch > 0xFFFD)) {
            result += c_replacementCharacter;
            ++index;
            continue;
        }

        switch (ch) {
            case L'&': result += L"&amp;"; break;
            case L'<': result += L"&lt;"; break;
            case L'>': result += L"&gt;"; break;
            case L'"': result += L"&quot;"; break;
            case L'\'': result += L"&apos;"; break;
            default: result += ch; break;
        }
        ++index;
    }
    return result;
}

} // namespace apc::toast
