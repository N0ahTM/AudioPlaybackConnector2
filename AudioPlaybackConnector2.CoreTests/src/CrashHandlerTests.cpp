#include "TestCheck.hpp"
#include <util/CrashHandler.hpp>
#include <util/Util.hpp>

#include <array>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <shellapi.h>
#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

namespace {
LONG WINAPI PreviousFilter(EXCEPTION_POINTERS*) {
    return EXCEPTION_CONTINUE_SEARCH;
}
void PreviousTerminate() {
    ExitProcess(91);
}
void PreviousInvalid(wchar_t const*, wchar_t const*, wchar_t const*, unsigned, uintptr_t) {}
void PreviousAbort(int) {}
std::string Read(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, {}};
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Isolated Native Crash Process /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunCrashHandlerChild() {
    int count = 0;
    wil::unique_hlocal_ptr<wchar_t*> arguments(CommandLineToArgvW(GetCommandLineW(), &count));
    if (!arguments || count != 4) return 80;
    const std::wstring_view mode(arguments.get()[2]);
    const std::filesystem::path root(arguments.get()[3]);
    if (!SetEnvironmentVariableW(L"TEMP", root.c_str()) || !SetEnvironmentVariableW(L"TMP", root.c_str())) return 81;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (mode == L"restore") {
        auto previousFilter = SetUnhandledExceptionFilter(PreviousFilter);
        auto previousTerminate = std::set_terminate(PreviousTerminate);
        auto previousInvalid = _set_invalid_parameter_handler(PreviousInvalid);
        auto previousAbort = std::signal(SIGABRT, PreviousAbort);
        for (int iteration = 0; iteration < 25; ++iteration) {
            {
                util::crash::CrashHandlers owner({});
                bool rejected = false;
                try {
                    util::crash::CrashHandlers duplicate({});
                } catch (std::logic_error const&) {
                    rejected = true;
                }
                Check(rejected, "a duplicate native registration cannot replace its owner");
            }
            Check(SetUnhandledExceptionFilter(PreviousFilter) == PreviousFilter, "restore previous SEH filter");
            Check(std::get_terminate() == PreviousTerminate, "restore previous terminate handler");
            Check(_get_invalid_parameter_handler() == PreviousInvalid, "restore previous invalid parameter handler");
            Check(std::signal(SIGABRT, PreviousAbort) == PreviousAbort, "restore previous abort handler");
        }
        SetUnhandledExceptionFilter(previousFilter);
        std::set_terminate(previousTerminate);
        _set_invalid_parameter_handler(previousInvalid);
        std::signal(SIGABRT, previousAbort);
        return g_failures.load();
    }

    util::EmergencyLog emergency;
    {
        util::Logger logger(root / L"emergency.log");
        logger.Sink().Write(L"native-crash-tail-marker");
        emergency = logger.Emergency();
        if (!logger.Shutdown(std::chrono::seconds(5))) return 82;
    }
    util::crash::CrashHandlers owner(emergency);
    if (mode == L"terminate") std::terminate();
    if (mode == L"abort") std::raise(SIGABRT);
    if (mode == L"invalid") _invalid_parameter_noinfo();
    if (mode == L"unhandled") RaiseException(0xE1234567, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    if (mode == L"vectored") RaiseException(STATUS_ILLEGAL_INSTRUCTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 83;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Native Registration and Emergency Artifacts ////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunCrashHandlerTests() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(L"APC-CrashTests-{}-{}", GetCurrentProcessId(), GetTickCount64());
    const auto executable = std::filesystem::path(wil::GetModuleFileNameW<std::wstring>());
    struct Case {
        wchar_t const* Mode;
        DWORD ExitCode;
        char const* CodeText;
    };
    constexpr std::array cases{
        Case{L"restore", 0, ""},
        Case{L"terminate", 0xE0000001, "E0000001"},
        Case{L"abort", 0xE0000003, "E0000003"},
        Case{L"invalid", 0xE0000002, "E0000002"},
        Case{L"unhandled", 0xE1234567, "E1234567"},
        Case{L"vectored", static_cast<DWORD>(STATUS_ILLEGAL_INSTRUCTION), "C000001D"},
    };
    for (auto const& test : cases) {
        const auto directory = root / test.Mode;
        std::filesystem::create_directories(directory);
        auto command =
            std::format(L"\"{}\" --crash-child {} \"{}\"", executable.wstring(), test.Mode, directory.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable.c_str(),
                             command.data(),
                             nullptr,
                             nullptr,
                             FALSE,
                             CREATE_NO_WINDOW,
                             nullptr,
                             nullptr,
                             &startup,
                             &process),
              "start isolated crash child");
        wil::unique_handle processHandle(process.hProcess);
        wil::unique_handle threadHandle(process.hThread);
        const auto wait = WaitForSingleObject(processHandle.get(), 30000);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(processHandle.get(), 84);
            WaitForSingleObject(processHandle.get(), 5000);
        }
        Check(wait == WAIT_OBJECT_0, "native crash handling terminates within the test deadline");
        DWORD exitCode = 0;
        Check(GetExitCodeProcess(processHandle.get(), &exitCode), "read child exit code");
        Check(exitCode == test.ExitCode, "native handler preserves the expected fatal exit code");
        if (exitCode == 0) continue;
        auto log = Read(directory / L"emergency.log");
        Check(log.find("BEGIN diagnostic tail") != std::string::npos, "native crash uses emergency dump");
        Check(log.find("native-crash-tail-marker", log.find("BEGIN diagnostic tail")) != std::string::npos,
              "native crash retains diagnostic tail after Logger destruction");
        Check(log.find(test.CodeText) != std::string::npos, "emergency record identifies the native exception");
        const auto reports = directory / L"AudioPlaybackConnector2" / L"CrashReports";
        std::size_t dumps = 0;
        std::size_t companions = 0;
        for (auto const& entry : std::filesystem::directory_iterator(reports)) {
            if (entry.path().extension() == L".dmp") {
                Check(Read(entry.path()).starts_with("MDMP"), "native crash writes a real minidump");
                ++dumps;
            }
            if (entry.path().extension() == L".txt") {
                Check(Read(entry.path()).find(test.CodeText) != std::string::npos,
                      "companion has the same exception code");
                ++companions;
            }
        }
        Check(dumps == 1 && companions == 1, "one fatal event writes one dump and one companion");
    }
    return g_failures.load();
}
