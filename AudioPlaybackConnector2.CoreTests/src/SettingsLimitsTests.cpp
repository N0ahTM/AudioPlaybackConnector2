#include <core/SettingsLimits.hpp>

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, char const* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

void TestUtf16Validation() {
    const std::wstring validPair{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
    const std::wstring highOnly{static_cast<wchar_t>(0xD83D)};
    const std::wstring lowOnly{static_cast<wchar_t>(0xDE00)};

    Check(apc::limits::IsValidUtf16(L"Audio") && apc::limits::IsValidUtf16(validPair), "valid UTF-16 must be accepted");
    Check(!apc::limits::IsValidUtf16(highOnly) && !apc::limits::IsValidUtf16(lowOnly),
          "unpaired UTF-16 surrogates must be rejected");
}

void TestBoundedStrings() {
    Check(apc::limits::IsBoundedUtf16(L"1234", 4), "a string at its limit must be accepted");
    Check(!apc::limits::IsBoundedUtf16(L"12345", 4), "a string beyond its limit must be rejected");

    const std::wstring embeddedNull{L'A', L'\0', L'B'};
    Check(!apc::limits::IsBoundedUtf16(embeddedNull, embeddedNull.size()), "embedded nulls must be rejected");
}

void TestSafeTruncation() {
    const std::wstring value{L'A', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00), L'B'};
    Check(apc::limits::TruncateUtf16(value, 2) == L"A", "truncation must not leave an unpaired high surrogate");
    Check(apc::limits::TruncateUtf16(value, 3) == value.substr(0, 3),
          "truncation must retain a complete surrogate pair");
}

void TestSupportedLanguages() {
    Check(apc::limits::IsSupportedLanguage(L"system") && apc::limits::IsSupportedLanguage(L"en") &&
              apc::limits::IsSupportedLanguage(L"zh_hant"),
          "supported languages must be accepted");
    Check(!apc::limits::IsSupportedLanguage(L"") && !apc::limits::IsSupportedLanguage(L"en-US"),
          "unsupported language identifiers must be rejected");
}

} // namespace

int RunSettingsLimitsTests() {
    TestUtf16Validation();
    TestBoundedStrings();
    TestSafeTruncation();
    TestSupportedLanguages();
    return g_failures;
}
