#pragma once

#include <core/SettingsData.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace apc::ui {
inline constexpr std::size_t c_diagnosticsLogTailBytes = 256 * 1024;
inline constexpr std::size_t c_diagnosticsLogLineBytes = 8 * 1024;

enum class DiagnosticsLogStatus { Available, Missing, Unavailable };

struct DiagnosticsLogResult {
    DiagnosticsLogStatus Status = DiagnosticsLogStatus::Missing;
    std::wstring DisplayPath;
    std::vector<std::wstring> Lines;
    bool SkippedMalformedOrOversized = false;
};

[[nodiscard]] DiagnosticsLogResult
CollectRecentDiagnosticLogLines(std::filesystem::path const& path,
                                SettingsData const& settings,
                                std::wstring_view redactedDevice,
                                std::wstring_view redactedValue,
                                std::size_t maxLines = 5,
                                std::size_t maxBytes = c_diagnosticsLogTailBytes) noexcept;
} // namespace apc::ui
