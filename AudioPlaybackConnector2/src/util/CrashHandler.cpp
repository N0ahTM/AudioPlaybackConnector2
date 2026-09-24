#include <util/CrashHandler.hpp>

#include <algorithm>
#include <iterator>
#include <cwchar>
#include <intrin.h>
#include <wil/resource.h>
#include <dbghelp.h>
#include <atomic>
#include <csignal>
#include <stdexcept>
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

struct CrashContext {
    explicit CrashContext(EmergencyLog emergency) : Emergency(std::move(emergency)) {}
    EmergencyLog Emergency;
    std::atomic_flag HandlingCrash = ATOMIC_FLAG_INIT;
    PVOID VectoredHandler = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
    std::terminate_handler PreviousTerminate = nullptr;
    _invalid_parameter_handler PreviousInvalidParameter = nullptr;
    void (*PreviousAbort)(int) = SIG_DFL;
};

// Native process callbacks cannot carry user data. This slot is only their
// registration context, never a service locator for normal application code.
std::atomic<CrashContext*> g_context = nullptr;
std::atomic<unsigned> g_entries = 0;

struct CallbackEntry {
    CallbackEntry() noexcept {
        // Sequential consistency orders admission before observing the slot,
        // including against the owner's close-and-drain sequence.
        g_entries.fetch_add(1);
        Context = g_context.load();
    }
    ~CallbackEntry() {
        if (g_entries.fetch_sub(1) == 1) g_entries.notify_all();
    }
    CrashContext* Context = nullptr;
};

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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Crash Processing //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline bool BuildCrashDirectoryPath(wchar_t* outDirectory, std::size_t outDirectoryCount) noexcept {
    if (!outDirectory || outDirectoryCount == 0) return false;

    wchar_t tempDirectory[MAX_PATH]{};
    DWORD tempLength = GetTempPathW(_countof(tempDirectory), tempDirectory);
    if (tempLength == 0 || tempLength >= _countof(tempDirectory)) {
        return false;
    }
    if (tempDirectory[tempLength - 1] == L'\\') {
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
                                         std::wstring_view originLabel,
                                         EmergencyLog const& emergency) noexcept {
    wchar_t origin[64]{};
    if (originLabel.empty()) {
        (void)wcscpy_s(origin, _countof(origin), L"unknown");
    } else {
        std::copy_n(originLabel.data(), std::min(originLabel.size(), std::size(origin) - 1), origin);
    }

    wchar_t dumpPath[MAX_PATH]{};
    MiniDumpWriteResult dumpResult{};
    if (BuildCrashDumpPath(dumpPath, _countof(dumpPath))) {
        dumpResult = WriteMiniDump(dumpPath, exceptionPointers);
    } else {
        dumpResult = {false, ERROR_PATH_NOT_FOUND};
    }

    // Best-effort: write a minimal companion .txt next to the .dmp so the next
    // app startup can show the crash dialog from a clean process state.
    if (dumpResult.Success && dumpPath[0] != L'\0') {
        wchar_t reportPath[MAX_PATH]{};
        (void)wcscpy_s(reportPath, _countof(reportPath), dumpPath);
        auto len = wcslen(reportPath);
        if (len > 4) {
            reportPath[len - 3] = L't';
            reportPath[len - 2] = L'x';
            reportPath[len - 1] = L't';
        }
        wchar_t reportContent[512]{};
        (void)swprintf_s(reportContent,
                         _countof(reportContent),
                         L"ExceptionCode: 0x%08lX\r\nOrigin: %ls\r\n",
                         exceptionCode,
                         origin);
        auto file = CreateFileW(
            reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            wil::unique_hfile h(file);
            char utf8[2048]{};
            auto length = WideCharToMultiByte(CP_UTF8, 0, reportContent, -1, utf8, sizeof(utf8), nullptr, nullptr);
            if (length > 1) {
                DWORD written = 0;
                (void)WriteFile(h.get(), utf8, static_cast<DWORD>(length - 1), &written, nullptr);
            }
        }
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

    char utf8[3072]{};
    auto length = WideCharToMultiByte(CP_UTF8, 0, debugLine, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (length > 1) (void)emergency.Write(std::string_view(utf8, static_cast<std::size_t>(length - 1)));
}

inline void
HandleFatalCrash(DWORD exceptionCode, EXCEPTION_POINTERS* exceptionPointers, std::wstring_view originLabel) noexcept {
    CallbackEntry entry;
    if (!entry.Context) return;
    auto& context = *entry.Context;
    DWORD safeExitCode = exceptionCode != 0 ? exceptionCode : 0xE0000000;
    if (!context.HandlingCrash.test_and_set()) {
        (void)context.Emergency.Dump(originLabel, safeExitCode);
        PersistCrashArtifactsMinimal(safeExitCode, exceptionPointers, originLabel, context.Emergency);
    }
    TerminateProcess(GetCurrentProcess(), safeExitCode);
    // Termination of our own process should succeed. Do not continue execution
    // or recursively invoke the installed CRT handler if the OS call fails.
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Exception Handlers ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS exceptionPointers) {
    if (!exceptionPointers || !exceptionPointers->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto const code = exceptionPointers->ExceptionRecord->ExceptionCode;
    // Access violations can be first-chance exceptions owned by injected hooks or UI/runtime internals.
    // Let normal SEH/UEF handling decide whether they are truly unhandled.
    if (code == STATUS_HEAP_CORRUPTION || code == STATUS_STACK_BUFFER_OVERRUN || code == STATUS_ILLEGAL_INSTRUCTION) {
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

CrashHandlers::CrashHandlers(EmergencyLog emergency)
    : m_context(std::make_unique<details::CrashContext>(std::move(emergency))) {
    details::CrashContext* expected = nullptr;
    if (!details::g_context.compare_exchange_strong(expected, m_context.get())) {
        throw std::logic_error("Crash handlers already have an application owner");
    }
    auto& context = *m_context;
    (void)HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    context.VectoredHandler = AddVectoredExceptionHandler(1, details::VectoredHandler);
    context.PreviousFilter = SetUnhandledExceptionFilter(details::UnhandledExceptionFilter);
    context.PreviousTerminate = std::set_terminate(details::TerminateHandler);
    context.PreviousInvalidParameter = _set_invalid_parameter_handler(details::InvalidParameterHandler);
    context.PreviousAbort = std::signal(SIGABRT, details::SignalAbortHandler);
}

CrashHandlers::~CrashHandlers() noexcept {
    details::g_context.store(nullptr);
    auto& context = *m_context;
    if (context.VectoredHandler) RemoveVectoredExceptionHandler(context.VectoredHandler);
    SetUnhandledExceptionFilter(context.PreviousFilter);
    std::set_terminate(context.PreviousTerminate);
    _set_invalid_parameter_handler(context.PreviousInvalidParameter);
    if (context.PreviousAbort != SIG_ERR) std::signal(SIGABRT, context.PreviousAbort);
    // Unregistration alone does not prove that an already entered callback has
    // returned. Keep its context alive until every admitted callback has left.
    auto entries = details::g_entries.load();
    while (entries != 0) {
        details::g_entries.wait(entries);
        entries = details::g_entries.load();
    }
}

} // namespace crash
} // namespace util
