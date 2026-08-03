#pragma once

#include <chrono>

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

inline std::filesystem::path GetModuleFsPath(HMODULE hModule) {
    std::wstring path(MAX_PATH, L'\0');
    DWORD actual = 0;
    while (true) {
        actual = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));
        THROW_LAST_ERROR_IF(actual == 0);
        if (static_cast<size_t>(actual) >= path.size())
            path.resize(path.size() * 2);
        else
            break;
    }
    path.resize(actual);
    return std::filesystem::path(path);
}

inline std::optional<std::vector<uint8_t>> LoadResourceData(HMODULE hInst, int id, const wchar_t* type) {
    auto hRes = FindResourceW(hInst, MAKEINTRESOURCEW(id), type);
    if (!hRes) return std::nullopt;
    auto size = SizeofResource(hInst, hRes);
    if (size == 0) return std::nullopt;
    auto hData = LoadResource(hInst, hRes);
    if (!hData) return std::nullopt;
    auto* ptr = static_cast<const uint8_t*>(LockResource(hData));
    if (!ptr) return std::nullopt;
    return std::vector<uint8_t>(ptr, ptr + size);
}

inline std::wstring ReplacePlaceholders(std::wstring_view templateStr, std::wstring_view replacement) {
    std::wstring result;
    size_t pos = 0;
    while (pos < templateStr.size()) {
        auto found = templateStr.find(L"{0}", pos);
        if (found == std::wstring_view::npos) {
            result.append(templateStr.substr(pos));
            break;
        }
        result.append(templateStr.substr(pos, found - pos));
        result.append(replacement);
        pos = found + 3;
    }
    return result;
}

} // namespace util
