#include <pch.h>
#include <util/CrashHandler.hpp>
#include <core/StringResources.hpp>

namespace util::crash {
namespace details {
constexpr std::size_t c_maxRetainedCrashReports = 5;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Getter / Setter ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring SafeResource(StringResources const& strings, std::string_view key, std::wstring_view fallback) {
    auto value = strings.Get(key);
    if (!value.empty()) {
        return std::wstring(value);
    }
    return std::wstring(fallback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Controller ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::filesystem::path GetCrashDirectory(std::filesystem::path const& baseLogPath) {
    if (baseLogPath.empty()) return {};
    auto dir = baseLogPath.parent_path() / L"CrashReports";
    return dir;
}

inline std::filesystem::path GetMinimalCrashDirectory() {
    std::wstring tempDirectory(MAX_PATH, L'\0');
    auto length = GetTempPathW(static_cast<DWORD>(tempDirectory.size()), tempDirectory.data());
    if (length == 0) return {};
    if (length >= tempDirectory.size()) {
        tempDirectory.resize(static_cast<size_t>(length) + 1);
        length = GetTempPathW(static_cast<DWORD>(tempDirectory.size()), tempDirectory.data());
        if (length == 0 || length >= tempDirectory.size()) return {};
    }
    tempDirectory.resize(length);
    return std::filesystem::path(tempDirectory) / L"AudioPlaybackConnector2" / L"CrashReports";
}

inline std::vector<std::filesystem::path> GetCrashDirectories(std::filesystem::path const& logPath) {
    std::vector<std::filesystem::path> directories;
    if (auto primary = GetCrashDirectory(logPath); !primary.empty()) {
        directories.push_back(std::move(primary));
    }
    if (auto minimal = GetMinimalCrashDirectory();
        !minimal.empty() &&
        std::ranges::none_of(directories, [&](auto const& existing) { return existing == minimal; })) {
        directories.push_back(std::move(minimal));
    }
    return directories;
}

inline void RetainNewestCrashReports(std::vector<std::filesystem::path> const& directories) noexcept {
    struct ArtifactSet {
        std::filesystem::path BasePath;
        std::vector<std::filesystem::path> Files;
        std::filesystem::file_time_type Newest{};
    };

    try {
        std::vector<ArtifactSet> artifacts;
        std::error_code ec;
        for (auto const& directory : directories) {
            if (!std::filesystem::exists(directory, ec)) {
                ec.clear();
                continue;
            }
            for (auto const& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) {
                    ec.clear();
                    continue;
                }
                const auto extension = entry.path().extension();
                if (extension != L".txt" && extension != L".dmp" && extension != L".prompted") continue;
                if (!entry.path().stem().wstring().starts_with(L"Crash_")) continue;

                auto basePath = entry.path();
                basePath.replace_extension();
                auto existing = std::ranges::find_if(
                    artifacts, [&](auto const& artifact) { return artifact.BasePath == basePath; });
                if (existing == artifacts.end()) {
                    artifacts.push_back(ArtifactSet{std::move(basePath)});
                    existing = std::prev(artifacts.end());
                }
                existing->Files.push_back(entry.path());
                auto modified = entry.last_write_time(ec);
                if (!ec && modified > existing->Newest) existing->Newest = modified;
                ec.clear();
            }
            ec.clear();
        }

        std::ranges::sort(artifacts, [](auto const& left, auto const& right) { return left.Newest > right.Newest; });
        for (std::size_t index = c_maxRetainedCrashReports; index < artifacts.size(); ++index) {
            for (auto const& file : artifacts[index].Files) {
                std::filesystem::remove(file, ec);
                ec.clear();
            }
        }
    } catch (...) {
    }
}

inline std::wstring UrlEncode(std::wstring_view text) {
    std::string utf8;
    try {
        utf8 = Utf16ToUtf8(text);
    } catch (...) {
        return {};
    }

    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    static constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return Utf8ToUtf16(encoded);
}

inline void ReplaceAll(std::wstring& value, std::wstring_view needle, std::wstring_view replacement) {
    std::size_t offset = 0;
    while (true) {
        offset = value.find(needle, offset);
        if (offset == std::wstring::npos) return;
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Environment / Metadata ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring GetWindowsVersionString() {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;

    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using RtlGetNtVersionNumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
        auto fn = reinterpret_cast<RtlGetNtVersionNumbersFn>(GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));
        if (fn) {
            fn(&major, &minor, &build);
            build &= 0xFFFF;
        }
    }

    if (major == 0 && minor == 0 && build == 0) {
        return L"unknown";
    }
    return std::format(L"{}.{}.{}", major, minor, build);
}

inline std::wstring GetAppVersionString() {
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD copied = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (copied == 0) return L"unknown";
    modulePath.resize(copied);

    auto versionDll = LoadLibraryW(L"version.dll");
    if (!versionDll) return L"unknown";
    auto releaseVersionDll = wil::scope_exit([&]() noexcept { FreeLibrary(versionDll); });

    using GetFileVersionInfoSizeWFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetFileVersionInfoWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using VerQueryValueWFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

    auto getFileVersionInfoSize =
        reinterpret_cast<GetFileVersionInfoSizeWFn>(GetProcAddress(versionDll, "GetFileVersionInfoSizeW"));
    auto getFileVersionInfo =
        reinterpret_cast<GetFileVersionInfoWFn>(GetProcAddress(versionDll, "GetFileVersionInfoW"));
    auto verQueryValue = reinterpret_cast<VerQueryValueWFn>(GetProcAddress(versionDll, "VerQueryValueW"));
    if (!getFileVersionInfoSize || !getFileVersionInfo || !verQueryValue) {
        return L"unknown";
    }

    DWORD unused = 0;
    DWORD versionSize = getFileVersionInfoSize(modulePath.c_str(), &unused);
    if (versionSize == 0) return L"unknown";

    std::vector<std::byte> versionData(versionSize);
    if (!getFileVersionInfo(modulePath.c_str(), 0, versionSize, versionData.data())) {
        return L"unknown";
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoLen = 0;
    if (!verQueryValue(versionData.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &fixedInfoLen) || !fixedInfo ||
        fixedInfoLen < sizeof(VS_FIXEDFILEINFO)) {
        return L"unknown";
    }

    return std::format(L"{}.{}.{}.{}",
                       HIWORD(fixedInfo->dwFileVersionMS),
                       LOWORD(fixedInfo->dwFileVersionMS),
                       HIWORD(fixedInfo->dwFileVersionLS),
                       LOWORD(fixedInfo->dwFileVersionLS));
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Artifact Writers //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline void WriteUtf8File(std::filesystem::path const& path, std::wstring_view contents) {
    auto file = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    wil::unique_hfile handle(file);

    std::string utf8;
    try {
        utf8 = Utf16ToUtf8(contents);
    } catch (...) {
        return;
    }
    if (utf8.empty()) return;

    DWORD written = 0;
    (void)WriteFile(handle.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

inline std::wstring ReadLogTail(std::filesystem::path const& logPath, std::size_t maxBytes) {
    if (logPath.empty() || maxBytes == 0) return {};
    auto file = CreateFileW(logPath.c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    wil::unique_hfile handle(file);

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(handle.get(), &fileSize) || fileSize.QuadPart <= 0) {
        return {};
    }

    std::uint64_t totalSize = static_cast<std::uint64_t>(fileSize.QuadPart);
    std::uint64_t bytesToRead = std::min<std::uint64_t>(totalSize, maxBytes);
    std::int64_t startOffset = static_cast<std::int64_t>(totalSize - bytesToRead);
    LARGE_INTEGER seek{};
    seek.QuadPart = startOffset;
    if (!SetFilePointerEx(handle.get(), seek, nullptr, FILE_BEGIN)) {
        return {};
    }

    std::string bytes(static_cast<std::size_t>(bytesToRead), '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr) ||
        bytesRead == 0) {
        return {};
    }
    bytes.resize(bytesRead);

    try {
        return Utf8ToUtf16(bytes);
    } catch (...) {
        return {};
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// User Reporting ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring BuildIssueUrl(DWORD exceptionCode,
                                  std::filesystem::path const& reportPath,
                                  std::filesystem::path const& dumpPath,
                                  std::filesystem::path const& logPath) {
    constexpr std::wstring_view c_issueBase = L"https://github.com/N0ahTM/AudioPlaybackConnector2/issues/new";
    auto title = std::format(L"Crash: 0x{:08X} in AudioPlaybackConnector2", exceptionCode);
    auto body = std::format(L"## Crash report\r\n"
                            L"- Exception code: `0x{:08X}`\r\n"
                            L"- App version: `{}`\r\n"
                            L"- Windows version: `{}`\r\n"
                            L"- Process id: `{}`\r\n"
                            L"\r\n"
                            L"## Local artifacts\r\n"
                            L"- Crash report: `{}`\r\n"
                            L"- Minidump: `{}`\r\n"
                            L"- App log: `{}`\r\n"
                            L"\r\n"
                            L"## What happened\r\n"
                            L"<!-- Briefly describe what happened right before the crash. -->\r\n",
                            exceptionCode,
                            GetAppVersionString(),
                            GetWindowsVersionString(),
                            GetCurrentProcessId(),
                            reportPath.wstring(),
                            dumpPath.wstring(),
                            logPath.wstring());

    return std::format(L"{}?title={}&body={}", c_issueBase, UrlEncode(title), UrlEncode(body));
}

inline void ShowCrashDialogAndOfferIssue(DWORD exceptionCode,
                                         std::filesystem::path const& reportPath,
                                         std::filesystem::path const& dumpPath,
                                         std::filesystem::path const& logPath,
                                         StringResources const& strings) {
    auto title = SafeResource(strings, "CrashDialogTitle", L"AudioPlaybackConnector2 crashed");
    auto body = SafeResource(
        strings,
        "CrashDialogBody",
        L"AudioPlaybackConnector2 crashed unexpectedly.\n\nError code: 0x{0:08X}\nCrash report: {1}\nMinidump: "
        L"{2}\n\nYes = Open prefilled GitHub issue\nNo = Open crash folder\nCancel = Close");
    auto exceptionCodeString = std::format(L"{:08X}", exceptionCode);
    ReplaceAll(body, L"{0:08X}", exceptionCodeString);
    ReplaceAll(body, L"{1}", reportPath.wstring());
    ReplaceAll(body, L"{2}", dumpPath.wstring());

    int result = MessageBoxW(
        nullptr, body.c_str(), title.c_str(), MB_YESNOCANCEL | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    if (result == IDYES) {
        auto issueUrl = BuildIssueUrl(exceptionCode, reportPath, dumpPath, logPath);
        ShellExecuteW(nullptr, L"open", issueUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (result == IDNO) {
        auto crashFolder = reportPath.empty() ? dumpPath.parent_path() : reportPath.parent_path();
        if (!crashFolder.empty()) {
            ShellExecuteW(nullptr, L"open", crashFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

} // namespace details

void CheckAndPromptCrashReports(std::filesystem::path const& logPath, StringResources const& strings) {
    try {
        auto crashDirectories = details::GetCrashDirectories(logPath);
        details::RetainNewestCrashReports(crashDirectories);

        std::vector<std::filesystem::path> reportFiles;
        std::error_code ec;
        for (auto const& crashDirectory : crashDirectories) {
            if (!std::filesystem::exists(crashDirectory, ec)) {
                ec.clear();
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(crashDirectory, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec) || entry.path().extension() != L".txt") {
                    ec.clear();
                    continue;
                }

                auto promptedMarker = entry.path();
                promptedMarker.replace_extension(L".prompted");
                if (!std::filesystem::exists(promptedMarker, ec)) {
                    reportFiles.push_back(entry.path());
                }
                ec.clear();
            }
            ec.clear();
        }

        if (reportFiles.empty()) {
            return;
        }

        std::sort(reportFiles.begin(), reportFiles.end(), [](const auto& a, const auto& b) {
            std::error_code ea, eb;
            return std::filesystem::last_write_time(a, ea) > std::filesystem::last_write_time(b, eb);
        });

        auto latestReport = reportFiles.front();
        auto latestDump = latestReport;
        latestDump.replace_extension(L".dmp");

        DWORD exceptionCode = 0;
        auto content = details::ReadLogTail(latestReport, 256);
        auto pos = content.find(L"ExceptionCode: 0x");
        if (pos != std::wstring::npos) {
            auto hexStart = pos + std::wstring_view(L"ExceptionCode: 0x").size();
            if (hexStart + 8 <= content.size()) {
                auto hexStr = content.substr(hexStart, 8);
                exceptionCode = static_cast<DWORD>(std::wcstoul(hexStr.c_str(), nullptr, 16));
            }
        }

        details::ShowCrashDialogAndOfferIssue(exceptionCode, latestReport, latestDump, logPath, strings);

        auto promptedMarker = latestReport;
        promptedMarker.replace_extension(L".prompted");
        details::WriteUtf8File(promptedMarker, L"prompted=true\r\n");
    } catch (...) {
        // Best-effort: crash-report UI must never block app launch.
    }
}

} // namespace util::crash
