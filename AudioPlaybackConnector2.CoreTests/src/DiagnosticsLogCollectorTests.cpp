#include <ui/DiagnosticsLogCollector.hpp>

#include <windows.h>
#include <winioctl.h>
#include <wil/resource.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class TemporaryFile {
public:
    TemporaryFile() {
        std::wstring directory(MAX_PATH, L'\0');
        auto const length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
        if (length == 0 || length >= directory.size()) throw std::runtime_error("GetTempPathW failed");
        directory.resize(length);
        std::wstring path(MAX_PATH, L'\0');
        if (GetTempFileNameW(directory.c_str(), L"apc", 0, path.data()) == 0) {
            throw std::runtime_error("GetTempFileNameW failed");
        }
        path.resize(std::wcslen(path.c_str()));
        m_path = std::move(path);
    }

    ~TemporaryFile() { DeleteFileW(m_path.c_str()); }

    TemporaryFile(TemporaryFile const&) = delete;
    TemporaryFile& operator=(TemporaryFile const&) = delete;

    [[nodiscard]] std::filesystem::path const& Path() const noexcept { return m_path; }

    void Write(std::string_view bytes) const {
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("opening temporary file failed");
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("writing temporary file failed");
    }

    void WriteSparseTail(std::uint64_t size, std::string_view tail) const {
        wil::unique_hfile file(CreateFileW(
            m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) throw std::runtime_error("opening sparse temporary file failed");
        DWORD ignored = 0;
        if (!DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr)) {
            throw std::runtime_error("marking temporary file sparse failed");
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(size - tail.size());
        if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
            throw std::runtime_error("seeking sparse temporary file failed");
        }
        DWORD written = 0;
        if (!WriteFile(file.get(), tail.data(), static_cast<DWORD>(tail.size()), &written, nullptr) ||
            written != static_cast<DWORD>(tail.size())) {
            throw std::runtime_error("writing sparse temporary file failed");
        }
    }

private:
    std::filesystem::path m_path;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::wstring name, std::wstring value) : m_name(std::move(name)) {
        std::wstring previous(32767, L'\0');
        auto const length =
            GetEnvironmentVariableW(m_name.c_str(), previous.data(), static_cast<DWORD>(previous.size()));
        if (length > 0 && length < previous.size()) {
            previous.resize(length);
            m_previous = std::move(previous);
            m_hadPrevious = true;
        }
        if (!SetEnvironmentVariableW(m_name.c_str(), value.c_str())) {
            throw std::runtime_error("setting environment variable failed");
        }
    }

    ~ScopedEnvironmentVariable() {
        SetEnvironmentVariableW(m_name.c_str(), m_hadPrevious ? m_previous.c_str() : nullptr);
    }

    ScopedEnvironmentVariable(ScopedEnvironmentVariable const&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable const&) = delete;

private:
    std::wstring m_name;
    std::wstring m_previous;
    bool m_hadPrevious = false;
};

void TestMissingAndUnavailableFiles() {
    TemporaryFile file;
    auto missingPath = file.Path();
    Check(DeleteFileW(missingPath.c_str()) != FALSE, "missing-file setup must remove the temporary file");
    auto missing = apc::ui::CollectRecentDiagnosticLogLines(missingPath, {}, L"device", L"value");
    Check(missing.Status == apc::ui::DiagnosticsLogStatus::Missing && missing.Lines.empty(),
          "a missing log must be reported without throwing");
    Check(missing.DisplayPath.starts_with(L"%TEMP%"), "the displayed log path must redact the temporary directory");

    TemporaryFile lockedFile;
    wil::unique_hfile lock(CreateFileW(lockedFile.Path().c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr));
    Check(static_cast<bool>(lock), "exclusive-file setup must acquire the file");
    auto unavailable = apc::ui::CollectRecentDiagnosticLogLines(lockedFile.Path(), {}, L"device", L"value");
    Check(unavailable.Status == apc::ui::DiagnosticsLogStatus::Unavailable && unavailable.Lines.empty(),
          "an exclusively locked log must be reported as unavailable");
}

void TestOnlyNewestRelevantLinesAreReturned() {
    TemporaryFile file;
    file.Write("ignored\nERROR one\nwarning two\nfailed three\ncrash four\nERROR five\nERROR six\n");
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), {}, L"device", L"value", 5);
    Check(result.Status == apc::ui::DiagnosticsLogStatus::Available, "a readable log must be available");
    Check(result.Lines.size() == 5, "only the configured number of recent lines may be returned");
    Check(!result.Lines.empty() && result.Lines.front().find(L"two") != std::wstring::npos,
          "the oldest retained line must be the second relevant line");
    Check(!result.Lines.empty() && result.Lines.back().find(L"six") != std::wstring::npos,
          "the newest relevant line must be retained");
}

void TestTailBoundaryAndMalformedLinesAreIsolated() {
    TemporaryFile file;
    std::string bytes(128, 'x');
    bytes += "\nERROR retained\nERROR malformed \xF0\x28\x8C\x28\nwarning final\n";
    file.Write(bytes);
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), {}, L"device", L"value", 5, 70);
    Check(result.Lines.size() == 2, "a malformed line must not discard neighboring valid lines");
    Check(result.SkippedMalformedOrOversized, "malformed input must be observable in the bounded result");
    Check(result.Lines[0].find(L"retained") != std::wstring::npos &&
              result.Lines[1].find(L"final") != std::wstring::npos,
          "the first partial tail line must be discarded and later valid lines retained");
}

void TestOversizedLinesAreDiscarded() {
    TemporaryFile file;
    std::string bytes = "ERROR ";
    bytes.append(apc::ui::c_diagnosticsLogLineBytes + 1, 'x');
    bytes += "\nwarning safe\n";
    file.Write(bytes);
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), {}, L"device", L"value");
    Check(result.SkippedMalformedOrOversized, "an oversized line must be marked as skipped");
    Check(result.Lines.size() == 1 && result.Lines[0].find(L"safe") != std::wstring::npos,
          "an oversized line must be dropped without hiding a later valid line");
}

void TestSensitiveValuesAreRedactedAndControlsNeutralized() {
    TemporaryFile file;
    file.Write("ERROR id=device-secret name=MY HEADSET alias=PRIVATE "
               "mac=AA:BB:CC:DD:EE:FF guid={12345678-1234-1234-1234-1234567890ab} \xE2\x80\xAEhidden\n");
    SettingsData settings;
    settings.Devices.push_back({L"Device-Secret", L"My Headset", L"Private"});
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), settings, L"[device]", L"[value]");
    Check(result.Lines.size() == 1, "the sensitive test line must be retained");
    if (result.Lines.empty()) return;
    auto const& line = result.Lines.front();
    Check(line.find(L"device-secret") == std::wstring::npos && line.find(L"MY HEADSET") == std::wstring::npos &&
              line.find(L"PRIVATE") == std::wstring::npos,
          "known values must be redacted case-insensitively");
    Check(line.find(L"AA:BB:CC:DD:EE:FF") == std::wstring::npos &&
              line.find(L"12345678-1234-1234-1234-1234567890ab") == std::wstring::npos,
          "generic hardware identifiers must be redacted");
    Check(line.find(static_cast<wchar_t>(0x202e)) == std::wstring::npos,
          "bidirectional control characters must be neutralized");
}

void TestRemovedDeviceFieldsAndLargeFilesRemainBounded() {
    TemporaryFile file;
    constexpr std::string_view tail =
        "discarded partial line\nERROR Snapshot device id=Bluetooth#Old_Device name=Former Headset isOpen=1\n";
    file.WriteSparseTail(100ULL * 1024 * 1024, tail);
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), {}, L"[device]", L"[value]");
    Check(result.Lines.size() == 1, "a bounded tail read must retain the newest relevant sparse-file line");
    if (result.Lines.empty()) return;
    auto const& line = result.Lines.front();
    Check(line.find(L"Bluetooth#Old_Device") == std::wstring::npos &&
              line.find(L"Former Headset") == std::wstring::npos,
          "structured fields from removed devices must be redacted without a settings entry");
    Check(line.find(L"id=[value]") != std::wstring::npos && line.find(L"name=[device]") != std::wstring::npos,
          "structured redaction must preserve useful field labels");
}

void TestCustomTemporaryDirectoryIsRedactedInsideLogLines() {
    TemporaryFile file;
    ScopedEnvironmentVariable temporaryDirectory(L"TEMP", L"X:\\PrivateTempRoot");
    file.Write("ERROR crash dump X:\\PrivateTempRoot\\AudioPlaybackConnector2\\crash.dmp\n");
    auto result = apc::ui::CollectRecentDiagnosticLogLines(file.Path(), {}, L"[device]", L"[value]");
    Check(result.Lines.size() == 1, "the custom temporary-directory test line must be retained");
    Check(!result.Lines.empty() && result.Lines.front().find(L"X:\\PrivateTempRoot") == std::wstring::npos &&
              result.Lines.front().find(L"%TEMP%") != std::wstring::npos,
          "custom temporary directories must be redacted inside copied log lines");
}
} // namespace

int RunDiagnosticsLogCollectorTests() {
    TestMissingAndUnavailableFiles();
    TestOnlyNewestRelevantLinesAreReturned();
    TestTailBoundaryAndMalformedLinesAreIsolated();
    TestOversizedLinesAreDiscarded();
    TestSensitiveValuesAreRedactedAndControlsNeutralized();
    TestRemovedDeviceFieldsAndLargeFilesRemainBounded();
    TestCustomTemporaryDirectoryIsRedactedInsideLogLines();
    return g_failures;
}
