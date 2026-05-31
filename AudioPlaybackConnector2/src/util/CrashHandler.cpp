#include <pch.h>
#include <util/CrashHandler.hpp>

#include <core/StringResources.hpp>

#include <dbghelp.h>
#include <csignal>
#include <exception>
#include <string_view>

namespace util {
namespace crash {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Interface /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace details {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Variables /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

constexpr DWORD c_exceptionCodeTerminate = 0xE0000001;
constexpr DWORD c_exceptionCodeInvalidParameter = 0xE0000002;
constexpr DWORD c_exceptionCodeSigAbort = 0xE0000003;

std::atomic<bool> g_installed = false;
std::atomic<bool> g_handlingCrash = false;
std::atomic<DWORD> g_lastExceptionCode = 0;
PVOID g_vectoredHandler = nullptr;
std::terminate_handler g_previousTerminate = nullptr;
_invalid_parameter_handler g_previousInvalidParameter = nullptr;
void (*g_previousSigAbrtHandler)(int) = nullptr;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Getter / Setter ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring SafeResource(std::string_view key, std::wstring_view fallback) {
    auto value = _(key);
    if (!value.empty()) {
        return std::wstring(value);
    }
    return std::wstring(fallback);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Controller ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring BuildTimestampForFilename() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format(L"{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}",
                       st.wYear,
                       st.wMonth,
                       st.wDay,
                       st.wHour,
                       st.wMinute,
                       st.wSecond,
                       st.wMilliseconds);
}

inline std::filesystem::path GetCrashDirectory() {
    auto baseLogPath = GetCachedLogPath();
    if (baseLogPath.empty()) return {};
    auto dir = baseLogPath.parent_path() / L"CrashReports";
    if (!util::details::EnsureDirectory(dir)) return {};
    return dir;
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

struct MiniDumpWriteResult {
    bool Success = false;
    DWORD ErrorCode = ERROR_GEN_FAILURE;
};

inline MiniDumpWriteResult WriteMiniDump(wchar_t const* dumpPath, EXCEPTION_POINTERS* exceptionPointers) {
    if (!dumpPath || dumpPath[0] == L'\0') return {false, ERROR_INVALID_PARAMETER};

    auto dbghelp = LoadLibraryW(L"DbgHelp.dll");
    if (!dbghelp) return {false, GetLastError()};
    auto releaseDbghelp = wil::scope_exit([&]() noexcept { FreeLibrary(dbghelp); });

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                              DWORD,
                                              HANDLE,
                                              MINIDUMP_TYPE,
                                              PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION,
                                              PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!fn) return {false, GetLastError()};

    auto dumpFile =
        CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE) return {false, GetLastError()};
    wil::unique_hfile dumpHandle(dumpFile);

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exceptionPointers;
    mei.ClientPointers = FALSE;

    auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                               MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);

    auto ok = fn(GetCurrentProcess(),
                 GetCurrentProcessId(),
                 dumpHandle.get(),
                 dumpType,
                 exceptionPointers ? &mei : nullptr,
                 nullptr,
                 nullptr);
    if (ok == TRUE) {
        return {true, ERROR_SUCCESS};
    }
    return {false, GetLastError()};
}

inline MiniDumpWriteResult WriteMiniDump(std::filesystem::path const& dumpPath, EXCEPTION_POINTERS* exceptionPointers) {
    return WriteMiniDump(dumpPath.c_str(), exceptionPointers);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// User Reporting ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline std::wstring
BuildIssueUrl(DWORD exceptionCode, std::filesystem::path const& reportPath, std::filesystem::path const& dumpPath) {
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
                            L"<!-- Bitte kurz beschreiben, was direkt vor dem Absturz passiert ist. -->\r\n",
                            exceptionCode,
                            GetAppVersionString(),
                            GetWindowsVersionString(),
                            GetCurrentProcessId(),
                            reportPath.wstring(),
                            dumpPath.wstring(),
                            GetCachedLogPath().wstring());

    return std::format(L"{}?title={}&body={}", c_issueBase, UrlEncode(title), UrlEncode(body));
}

inline void ShowCrashDialogAndOfferIssue(DWORD exceptionCode,
                                         std::filesystem::path const& reportPath,
                                         std::filesystem::path const& dumpPath) {
    auto title = SafeResource("CrashDialogTitle", L"AudioPlaybackConnector2 crashed");
    auto body = SafeResource(
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
        auto issueUrl = BuildIssueUrl(exceptionCode, reportPath, dumpPath);
        ShellExecuteW(nullptr, L"open", issueUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (result == IDNO) {
        auto crashFolder = reportPath.empty() ? dumpPath.parent_path() : reportPath.parent_path();
        if (!crashFolder.empty()) {
            ShellExecuteW(nullptr, L"open", crashFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Crash Processing //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline void
PersistCrashArtifacts(DWORD exceptionCode, EXCEPTION_POINTERS* exceptionPointers, std::wstring_view originLabel) {
    auto crashDir = GetCrashDirectory();
    auto timestamp = BuildTimestampForFilename();
    auto reportPath = crashDir.empty() ? std::filesystem::path{} : (crashDir / std::format(L"Crash_{}.txt", timestamp));
    auto dumpPath = crashDir.empty() ? std::filesystem::path{} : (crashDir / std::format(L"Crash_{}.dmp", timestamp));

    MiniDumpWriteResult dumpResult{};
    if (!dumpPath.empty()) {
        dumpResult = WriteMiniDump(dumpPath, exceptionPointers);
    }

    auto report = std::format(
        L"AudioPlaybackConnector2 crash report\r\n"
        L"===============================\r\n"
        L"Timestamp: {}\r\n"
        L"Origin: {}\r\n"
        L"ExceptionCode: 0x{:08X}\r\n"
        L"ThreadId: {}\r\n"
        L"ProcessId: {}\r\n"
        L"AppVersion: {}\r\n"
        L"WindowsVersion: {}\r\n"
        L"Executable: {}\r\n"
        L"LogPath: {}\r\n"
        L"MiniDump: {}\r\n",
        timestamp,
        originLabel,
        exceptionCode,
        GetCurrentThreadId(),
        GetCurrentProcessId(),
        GetAppVersionString(),
        GetWindowsVersionString(),
        util::GetModuleFsPath(nullptr).wstring(),
        GetCachedLogPath().wstring(),
        dumpResult.Success
            ? dumpPath.wstring()
            : std::format(L"<failed (GetLastError={} / 0x{:08X})>", dumpResult.ErrorCode, dumpResult.ErrorCode));

    auto logTail = ReadLogTail(GetCachedLogPath(), 16 * 1024);
    if (!logTail.empty()) {
        report += L"\r\nRecentLogTail:\r\n--------------\r\n";
        report += logTail;
        if (!report.empty() && report.back() != L'\n') {
            report += L"\r\n";
        }
    }

    if (!reportPath.empty()) {
        WriteUtf8File(reportPath, report);
    }

    DebugTrace(L"[CrashHandler] Fatal crash captured: origin={0} code=0x{1:08X} report={2} dump={3}",
               std::wstring(originLabel),
               exceptionCode,
               reportPath.wstring(),
               dumpResult.Success ? dumpPath.wstring() : std::format(L"<failed 0x{:08X}>", dumpResult.ErrorCode));

    ShowCrashDialogAndOfferIssue(exceptionCode, reportPath, dumpPath);
}

inline bool BuildCrashDirectoryPath(wchar_t* outDirectory, std::size_t outDirectoryCount) noexcept {
    if (!outDirectory || outDirectoryCount == 0) return false;

    wchar_t tempDirectory[MAX_PATH]{};
    DWORD tempLength = GetTempPathW(_countof(tempDirectory), tempDirectory);
    if (tempLength == 0 || tempLength >= _countof(tempDirectory)) {
        return false;
    }
    if (tempLength > 0 && tempDirectory[tempLength - 1] == L'\\') {
        tempDirectory[tempLength - 1] = L'\0';
    }

    wchar_t appDirectory[MAX_PATH]{};
    if (swprintf_s(appDirectory, _countof(appDirectory), L"%ls\\AudioPlaybackConnector2", tempDirectory) <= 0) {
        return false;
    }
    (void)CreateDirectoryW(appDirectory, nullptr);

    if (swprintf_s(outDirectory, outDirectoryCount, L"%ls\\CrashReports", appDirectory) <= 0) {
        return false;
    }
    (void)CreateDirectoryW(outDirectory, nullptr);

    auto attrs = GetFileAttributesW(outDirectory);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool BuildCrashDumpPath(wchar_t* outPath, std::size_t outPathCount) noexcept {
    if (!outPath || outPathCount == 0) return false;

    wchar_t crashDirectory[MAX_PATH]{};
    if (!BuildCrashDirectoryPath(crashDirectory, _countof(crashDirectory))) {
        return false;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    return swprintf_s(outPath,
                      outPathCount,
                      L"%ls\\Crash_%04u%02u%02u_%02u%02u%02u_%03u.dmp",
                      crashDirectory,
                      st.wYear,
                      st.wMonth,
                      st.wDay,
                      st.wHour,
                      st.wMinute,
                      st.wSecond,
                      st.wMilliseconds) > 0;
}

inline void PersistCrashArtifactsMinimal(DWORD exceptionCode,
                                         EXCEPTION_POINTERS* exceptionPointers,
                                         std::wstring_view originLabel) noexcept {
    wchar_t origin[64]{};
    if (originLabel.empty()) {
        (void)wcscpy_s(origin, _countof(origin), L"unknown");
    } else {
        (void)wcsncpy_s(origin, _countof(origin), originLabel.data(), _TRUNCATE);
    }

    wchar_t dumpPath[MAX_PATH]{};
    MiniDumpWriteResult dumpResult{};
    if (BuildCrashDumpPath(dumpPath, _countof(dumpPath))) {
        dumpResult = WriteMiniDump(dumpPath, exceptionPointers);
    } else {
        dumpResult = {false, ERROR_PATH_NOT_FOUND};
    }

    wchar_t debugLine[768]{};
    if (dumpResult.Success) {
        (void)swprintf_s(debugLine,
                         _countof(debugLine),
                         L"[CrashHandler] Fatal crash origin=%ls code=0x%08lX dump=%ls\r\n",
                         origin,
                         exceptionCode,
                         dumpPath);
    } else {
        (void)swprintf_s(debugLine,
                         _countof(debugLine),
                         L"[CrashHandler] Fatal crash origin=%ls code=0x%08lX dump-write-failed=0x%08lX\r\n",
                         origin,
                         exceptionCode,
                         dumpResult.ErrorCode);
    }
    OutputDebugStringW(debugLine);
}

inline void
HandleFatalCrash(DWORD exceptionCode, EXCEPTION_POINTERS* exceptionPointers, std::wstring_view originLabel) {
    DWORD safeExitCode = exceptionCode != 0 ? exceptionCode : 0xE0000000;
    g_lastExceptionCode.store(safeExitCode, std::memory_order_relaxed);

    bool expected = false;
    if (!g_handlingCrash.compare_exchange_strong(expected, true)) {
        TerminateProcess(GetCurrentProcess(), safeExitCode);
    }

    // Keep fatal-exception handling minimal and best-effort: avoid std::format,
    // file tail reads, UI dialogs, and shell calls in potentially corrupted state.
    PersistCrashArtifactsMinimal(safeExitCode, exceptionPointers, originLabel);
    TerminateProcess(GetCurrentProcess(), safeExitCode);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Exception Handlers ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS exceptionPointers) {
    if (!exceptionPointers || !exceptionPointers->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto const code = exceptionPointers->ExceptionRecord->ExceptionCode;
    if (code == STATUS_HEAP_CORRUPTION || code == STATUS_STACK_BUFFER_OVERRUN || code == STATUS_ACCESS_VIOLATION ||
        code == STATUS_ILLEGAL_INSTRUCTION) {
        HandleFatalCrash(code, exceptionPointers, L"VectoredException");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS exceptionPointers) {
    DWORD code = exceptionPointers && exceptionPointers->ExceptionRecord
                     ? exceptionPointers->ExceptionRecord->ExceptionCode
                     : 0xE0000000;
    HandleFatalCrash(code, exceptionPointers, L"UnhandledExceptionFilter");
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void TerminateHandler() {
    HandleFatalCrash(c_exceptionCodeTerminate, nullptr, L"std::terminate");
}

inline void InvalidParameterHandler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t) {
    HandleFatalCrash(c_exceptionCodeInvalidParameter, nullptr, L"InvalidParameterHandler");
}

inline void SignalAbortHandler(int signalValue) {
    if (signalValue == SIGABRT) {
        HandleFatalCrash(c_exceptionCodeSigAbort, nullptr, L"SIGABRT");
    }
}

} // namespace details

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void InstallCrashHandlers() {
    bool expected = false;
    if (!details::g_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    (void)HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    details::g_vectoredHandler = AddVectoredExceptionHandler(1, details::VectoredHandler);
    SetUnhandledExceptionFilter(details::UnhandledExceptionFilter);
    details::g_previousTerminate = std::set_terminate(details::TerminateHandler);
    details::g_previousInvalidParameter = _set_invalid_parameter_handler(details::InvalidParameterHandler);
    details::g_previousSigAbrtHandler = std::signal(SIGABRT, details::SignalAbortHandler);
    DebugTrace(L"[CrashHandler] Installed global crash handlers");
}

} // namespace crash
} // namespace util
