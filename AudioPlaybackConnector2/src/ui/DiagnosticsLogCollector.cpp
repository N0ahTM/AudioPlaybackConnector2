#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wil/resource.h>

#include <ui/DiagnosticsLogCollector.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <limits>
#include <optional>
#include <regex>

namespace {
using apc::ui::DiagnosticsLogResult;
using apc::ui::DiagnosticsLogStatus;

std::wstring EnvironmentValue(std::wstring_view name) {
    std::wstring buffer(32767, L'\0');
    auto const length =
        GetEnvironmentVariableW(std::wstring(name).c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

bool ContainsRelevantKeyword(std::string_view line) noexcept {
    constexpr std::array<std::string_view, 4> keywords{"error", "failed", "warning", "crash"};
    return std::ranges::any_of(keywords, [&](std::string_view keyword) {
        return std::search(line.begin(), line.end(), keyword.begin(), keyword.end(), [](char left, char right) {
                   auto const folded = [](char value) noexcept {
                       auto const byte = static_cast<unsigned char>(value);
                       return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte + ('a' - 'A')) : byte;
                   };
                   return folded(left) == folded(right);
               }) != line.end();
    });
}

std::optional<std::wstring> DecodeUtf8Line(std::string_view line) {
    if (line.empty()) return std::wstring{};
    if (line.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    auto const length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (length == 0) return std::nullopt;
    std::wstring decoded(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), decoded.data(), length) == 0) {
        return std::nullopt;
    }
    return decoded;
}

void SanitizeControls(std::wstring& line) noexcept {
    for (auto& ch : line) {
        auto const code = static_cast<std::uint32_t>(ch);
        auto const disallowedControl = (code < 0x20 && ch != L'\t') || (code >= 0x7f && code <= 0x9f);
        auto const bidiControl = (code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069) ||
                                 code == 0x200e || code == 0x200f || code == 0x061c;
        if (disallowedControl || bidiControl) ch = L' ';
    }
}

bool ReplaceAllInsensitiveBounded(std::wstring& value,
                                  std::wstring_view needle,
                                  std::wstring_view replacement,
                                  std::size_t maxCharacters) {
    if (needle.empty()) return true;
    auto const equalsInsensitive = [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    };

    auto searchFrom = value.begin();
    while (searchFrom != value.end()) {
        auto const found = std::search(searchFrom, value.end(), needle.begin(), needle.end(), equalsInsensitive);
        if (found == value.end()) break;
        auto const offset = static_cast<std::size_t>(std::distance(value.begin(), found));
        if (replacement.size() > maxCharacters || value.size() - needle.size() > maxCharacters - replacement.size()) {
            return false;
        }
        value.replace(offset, needle.size(), replacement);
        if (value.size() > maxCharacters) return false;
        searchFrom = value.begin() + static_cast<std::ptrdiff_t>(offset + replacement.size());
    }
    return true;
}

struct Replacement {
    std::wstring Value;
    std::wstring ReplacementText;
};

std::vector<Replacement>
BuildReplacements(SettingsData const& settings, std::wstring_view redactedDevice, std::wstring_view redactedValue) {
    std::vector<Replacement> replacements;
    replacements.reserve(4 + settings.Devices.size() * 3);
    auto add = [&](std::wstring value, std::wstring_view replacement) {
        if (!value.empty()) replacements.push_back({std::move(value), std::wstring(replacement)});
    };
    add(EnvironmentValue(L"LOCALAPPDATA"), L"%LOCALAPPDATA%");
    add(EnvironmentValue(L"USERPROFILE"), L"%USERPROFILE%");
    add(EnvironmentValue(L"TEMP"), L"%TEMP%");
    add(EnvironmentValue(L"TMP"), L"%TEMP%");
    for (auto const& device : settings.Devices) {
        add(device.Id, redactedValue);
        add(device.Name, redactedDevice);
        add(device.Alias, redactedDevice);
    }
    std::ranges::sort(replacements,
                      [](auto const& left, auto const& right) { return left.Value.size() > right.Value.size(); });
    return replacements;
}

std::wstring BuildRedactedDisplayPath(std::filesystem::path const& path, std::wstring_view redactedValue) {
    auto value = path.wstring();
    std::vector<Replacement> replacements;
    auto add = [&](std::wstring_view environmentName, std::wstring_view placeholder) {
        auto environmentValue = EnvironmentValue(environmentName);
        if (!environmentValue.empty()) {
            replacements.push_back({std::move(environmentValue), std::wstring(placeholder)});
        }
    };
    add(L"LOCALAPPDATA", L"%LOCALAPPDATA%");
    add(L"USERPROFILE", L"%USERPROFILE%");
    add(L"TEMP", L"%TEMP%");
    add(L"TMP", L"%TEMP%");
    std::ranges::sort(replacements,
                      [](auto const& left, auto const& right) { return left.Value.size() > right.Value.size(); });
    for (auto const& replacement : replacements) {
        if (!ReplaceAllInsensitiveBounded(value, replacement.Value, replacement.ReplacementText, 32767)) {
            value.clear();
            break;
        }
    }
    if (!value.empty() && value == path.wstring()) {
        value = std::wstring(redactedValue) + L"\\" + path.filename().wstring();
    }
    return value;
}

std::optional<std::wstring> RedactLine(std::wstring line,
                                       std::vector<Replacement> const& replacements,
                                       std::wstring_view redactedValue,
                                       std::wstring_view redactedDevice,
                                       std::size_t maxCharacters) {
    SanitizeControls(line);
    for (auto const& replacement : replacements) {
        if (!ReplaceAllInsensitiveBounded(line, replacement.Value, replacement.ReplacementText, maxCharacters)) {
            return std::nullopt;
        }
    }

    static std::wregex const structuredIdPattern(LR"((\bid=)[^\s,]+)", std::regex_constants::icase);
    static std::wregex const structuredNamePattern(LR"((\bname=).*?(?= isOpen=))", std::regex_constants::icase);
    static std::wregex const macPattern(LR"((?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})");
    static std::wregex const bluetoothDevicePattern(LR"((?:Bluetooth#|BTHENUM\\)[^\s,]+)", std::regex_constants::icase);
    static std::wregex const guidPattern(
        LR"(\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}?)");
    line = std::regex_replace(line, structuredIdPattern, std::wstring(L"$1") + std::wstring(redactedValue));
    line = std::regex_replace(line, structuredNamePattern, std::wstring(L"$1") + std::wstring(redactedDevice));
    line = std::regex_replace(line, macPattern, std::wstring(redactedValue));
    line = std::regex_replace(line, bluetoothDevicePattern, std::wstring(redactedValue));
    line = std::regex_replace(line, guidPattern, std::wstring(redactedValue));
    if (line.size() > maxCharacters) return std::nullopt;
    return line;
}
} // namespace

DiagnosticsLogResult apc::ui::CollectRecentDiagnosticLogLines(std::filesystem::path const& path,
                                                              SettingsData const& settings,
                                                              std::wstring_view redactedDevice,
                                                              std::wstring_view redactedValue,
                                                              std::size_t maxLines,
                                                              std::size_t maxBytes) noexcept {
    DiagnosticsLogResult result;
    try {
        result.DisplayPath = BuildRedactedDisplayPath(path, redactedValue);
        if (path.empty() || maxLines == 0 || maxBytes == 0) return result;
        maxBytes = std::min(maxBytes, c_diagnosticsLogTailBytes);
        static_assert(c_diagnosticsLogTailBytes <= std::numeric_limits<DWORD>::max());

        wil::unique_hfile file(CreateFileW(path.c_str(),
                                           GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                           nullptr));
        if (!file) {
            auto const error = GetLastError();
            result.Status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                                ? DiagnosticsLogStatus::Missing
                                : DiagnosticsLogStatus::Unavailable;
            return result;
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result.Status = DiagnosticsLogStatus::Unavailable;
            return result;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
            result.Status = DiagnosticsLogStatus::Unavailable;
            return result;
        }
        result.Status = DiagnosticsLogStatus::Available;
        auto const bytesToRead =
            static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(size.QuadPart), maxBytes));
        if (bytesToRead == 0) return result;

        auto const offset = size.QuadPart - static_cast<LONGLONG>(bytesToRead);
        LARGE_INTEGER position{};
        position.QuadPart = offset;
        if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
            result.Status = DiagnosticsLogStatus::Unavailable;
            return result;
        }

        std::string bytes(bytesToRead, '\0');
        std::size_t totalRead = 0;
        while (totalRead < bytes.size()) {
            DWORD read = 0;
            auto const remaining = static_cast<DWORD>(bytes.size() - totalRead);
            if (!ReadFile(file.get(), bytes.data() + totalRead, remaining, &read, nullptr)) {
                result.Status = DiagnosticsLogStatus::Unavailable;
                result.Lines.clear();
                return result;
            }
            if (read == 0) break;
            totalRead += read;
        }
        bytes.resize(totalRead);

        std::size_t start = 0;
        if (offset > 0) {
            auto const newline = bytes.find('\n');
            if (newline == std::string::npos) return result;
            start = newline + 1;
        } else if (bytes.starts_with("\xEF\xBB\xBF")) {
            start = 3;
        }

        std::deque<std::string> recentRawLines;
        while (start <= bytes.size()) {
            auto const newline = bytes.find('\n', start);
            auto const end = newline == std::string::npos ? bytes.size() : newline;
            auto const length = end - start;
            auto line = std::string_view(bytes).substr(start, length);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.size() > c_diagnosticsLogLineBytes || line.find('\0') != std::string_view::npos) {
                result.SkippedMalformedOrOversized = true;
            } else if (ContainsRelevantKeyword(line)) {
                recentRawLines.emplace_back(line);
                if (recentRawLines.size() > maxLines) recentRawLines.pop_front();
            }
            if (newline == std::string::npos) break;
            start = newline + 1;
        }

        auto const replacements = BuildReplacements(settings, redactedDevice, redactedValue);
        result.Lines.reserve(recentRawLines.size());
        for (auto const& rawLine : recentRawLines) {
            auto decoded = DecodeUtf8Line(rawLine);
            if (!decoded) {
                result.SkippedMalformedOrOversized = true;
                continue;
            }
            auto redacted =
                RedactLine(std::move(*decoded), replacements, redactedValue, redactedDevice, c_diagnosticsLogLineBytes);
            if (!redacted) {
                result.SkippedMalformedOrOversized = true;
                continue;
            }
            result.Lines.push_back(std::move(*redacted));
        }
    } catch (...) {
        result.Status = DiagnosticsLogStatus::Unavailable;
        result.Lines.clear();
    }
    return result;
}
