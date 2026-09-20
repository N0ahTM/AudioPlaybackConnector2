#pragma once

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <windows.h>
#include <wil/result.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// String Helpers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

inline std::wstring Utf8ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for MultiByteToWideChar");
    }
    const int len =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(len == 0);
    std::wstring out(len, L'\0');
    THROW_LAST_ERROR_IF(
        0 == MultiByteToWideChar(
                 CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), len));
    return out;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for WideCharToMultiByte");
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(len == 0);
    std::string out(len, '\0');
    THROW_LAST_ERROR_IF(0 == WideCharToMultiByte(CP_UTF8,
                                                 WC_ERR_INVALID_CHARS,
                                                 utf16.data(),
                                                 static_cast<int>(utf16.size()),
                                                 out.data(),
                                                 len,
                                                 nullptr,
                                                 nullptr));
    return out;
}

} // namespace util
