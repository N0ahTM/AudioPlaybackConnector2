#include <control/Win32CommandTransport.hpp>
#include <control/CommandPipeIo.hpp>

#include <appmodel.h>
#include <shellapi.h>
#include <wil/resource.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace apc::control::client {
namespace {

constexpr DWORD c_retryIntervalMs = 200;
constexpr DWORD c_pipeExchangeTimeoutMs = 42000;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Server Identity ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// The retained process handle prevents a PID-reuse match during an indeterminate replay.
struct Win32ServerIdentity final : ServerIdentity {
    DWORD ProcessId = 0;
    FILETIME CreationTime{};
    wil::unique_handle Process;
};

std::optional<std::wstring> ExpectedUnpackagedServerPath() {
    DWORD capacity = 260;
    for (;;) {
        std::wstring modulePath(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), capacity);
        if (length == 0) return std::nullopt;
        if (length < capacity) {
            modulePath.resize(length);
            const auto separator = modulePath.find_last_of(L"\\/");
            if (separator == std::wstring::npos) return std::nullopt;
            modulePath.resize(separator + 1);
            modulePath += L"AudioPlaybackConnector2\\AudioPlaybackConnector2.exe";
            return modulePath;
        }
        if (capacity >= 32'768) return std::nullopt;
        capacity = std::min<DWORD>(capacity * 2, 32'768);
    }
}

std::shared_ptr<Win32ServerIdentity>
CaptureServerIdentity(HANDLE pipe,
                      std::optional<apc::control::ExecutableFileIdentity> const& expectedUnpackagedServerIdentity) {
    ULONG processId = 0;
    if (!GetNamedPipeServerProcessId(pipe, &processId) || processId == 0) return {};

    wil::unique_handle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)));
    if (!process.get()) return {};
    if (!apc::control::IsTrustedPeerProcess(
            process.get(), static_cast<DWORD>(processId), expectedUnpackagedServerIdentity)) {
        return {};
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process.get(), &creation, &exit, &kernel, &user) ||
        WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
        return {};
    }
    auto identity = std::make_shared<Win32ServerIdentity>();
    identity->ProcessId = static_cast<DWORD>(processId);
    identity->CreationTime = creation;
    identity->Process = std::move(process);
    return identity;
}

bool IsSameLiveServer(Win32ServerIdentity const& expected, Win32ServerIdentity const& observed) noexcept {
    return expected.ProcessId == observed.ProcessId &&
           CompareFileTime(&expected.CreationTime, &observed.CreationTime) == 0 && expected.Process.get() &&
           WaitForSingleObject(expected.Process.get(), 0) == WAIT_TIMEOUT;
}

std::optional<std::wstring> CurrentPackageFamilyName() {
    UINT32 length = 0;
    const LONG initial = GetCurrentPackageFamilyName(&length, nullptr);
    if (initial != ERROR_INSUFFICIENT_BUFFER || length == 0) return std::nullopt;

    std::wstring familyName(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, familyName.data()) != ERROR_SUCCESS || length == 0) return std::nullopt;
    familyName.resize(length - 1);
    return familyName;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Transport /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

Win32CommandTransport::Win32CommandTransport()
    : m_pipeName(apc::control::PipeName()), m_expectedUnpackagedServerIdentity([] {
          auto path = ExpectedUnpackagedServerPath();
          return path ? apc::control::ExecutableIdentityFromPath(*path) : std::nullopt;
      }()) {}

AttemptResult Win32CommandTransport::TrySendOnce(Request const& request,
                                                 Response& response,
                                                 DWORD waitMs,
                                                 std::uint64_t overallDeadline,
                                                 ServerIdentityPtr& observedServer,
                                                 ServerIdentityPtr const& expectedServer) {
    if (!m_pipeName) return AttemptResult::NotConnected;
    auto expected = std::dynamic_pointer_cast<Win32ServerIdentity const>(expectedServer);
    if (expectedServer && !expected) return AttemptResult::ServerChanged;
    observedServer.reset();
    const auto connectionDeadline = std::min(apc::control::DeadlineAfter(waitMs), overallDeadline);
    const auto firstInstance = static_cast<std::size_t>(request.CorrelationId.Low % apc::control::c_pipeInstanceCount);
    bool sawRejectedEndpoint = false;
    bool sawDifferentServer = false;

    while (true) {
        for (std::size_t offset = 0; offset < apc::control::c_pipeInstanceCount; ++offset) {
            const auto instance = (firstInstance + offset) % apc::control::c_pipeInstanceCount;
            const auto instanceName = apc::control::PipeInstanceName(*m_pipeName, instance);
            wil::unique_handle pipe(CreateFileW(instanceName.c_str(),
                                                GENERIC_READ | FILE_WRITE_DATA,
                                                0,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                                nullptr));
            if (!pipe) {
                const auto error = GetLastError();
                if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) sawRejectedEndpoint = true;
                continue;
            }
            auto currentServer = CaptureServerIdentity(pipe.get(), m_expectedUnpackagedServerIdentity);
            if (!currentServer) {
                sawRejectedEndpoint = true;
                continue;
            }
            if (expected && !IsSameLiveServer(*expected, *currentServer)) {
                sawDifferentServer = true;
                continue;
            }
            observedServer = currentServer;

            const auto exchangeDeadline =
                std::min(apc::control::DeadlineAfter(c_pipeExchangeTimeoutMs), overallDeadline);
            if (apc::control::RemainingWait(exchangeDeadline) == 0) {
                return apc::control::client::AttemptResult::NotConnected;
            }
            if (apc::control::WriteRequest(pipe.get(), request, nullptr, exchangeDeadline) !=
                apc::control::IoStatus::Success) {
                return apc::control::client::AttemptResult::Indeterminate;
            }
            if (apc::control::ReadResponse(pipe.get(), response, nullptr, exchangeDeadline) !=
                    apc::control::IoStatus::Success ||
                response.CorrelationId != request.CorrelationId) {
                return apc::control::client::AttemptResult::Indeterminate;
            }
            (void)apc::control::WriteAcknowledgement(pipe.get(), request.CorrelationId, nullptr, exchangeDeadline);
            return apc::control::client::AttemptResult::Complete;
        }

        if (apc::control::RemainingWait(connectionDeadline) == 0) {
            if (sawDifferentServer) return apc::control::client::AttemptResult::ServerChanged;
            return sawRejectedEndpoint ? apc::control::client::AttemptResult::Rejected
                                       : apc::control::client::AttemptResult::NotConnected;
        }
        Sleep(std::min<DWORD>(c_retryIntervalMs, apc::control::RemainingWait(connectionDeadline)));
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Packaged App Activation ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool Win32CommandTransport::LaunchPackagedApp() {
    auto familyName = CurrentPackageFamilyName();
    if (!familyName) return false;

    auto target = L"shell:AppsFolder\\" + *familyName + L"!App";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) == TRUE;
}

} // namespace apc::control::client
