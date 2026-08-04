#ifndef APC_COMMAND_PIPE_SERVER_STANDALONE
#include <pch.h>
#endif

#include <control/CommandPipeSecurity.hpp>
#include <services/CommandLineControlServer.hpp>
#include <util/RuntimeApartment.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {
enum class PipePhase {
    Disconnected,
    Connecting,
    ReadingRequestHeader,
    ReadingRequestPayload,
    RunningHandler,
    WritingResponseHeader,
    WritingResponsePayload,
    ReadingAcknowledgement
};

thread_local CommandLineControlServer* g_activeHandlerServer = nullptr;

std::optional<std::wstring> ExpectedUnpackagedControlPath() {
    DWORD capacity = 260;
    std::wstring modulePath;
    for (;;) {
        modulePath.assign(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), capacity);
        if (length == 0) return std::nullopt;
        if (length < capacity) {
            modulePath.resize(length);
            break;
        }
        if (capacity >= 32'768) return std::nullopt;
        capacity = std::min<DWORD>(capacity * 2, 32'768);
    }

    const auto executableSeparator = modulePath.find_last_of(L"\\/");
    if (executableSeparator == std::wstring::npos) return std::nullopt;
    modulePath.resize(executableSeparator);
    const auto applicationDirectorySeparator = modulePath.find_last_of(L"\\/");
    if (applicationDirectorySeparator == std::wstring::npos) return std::nullopt;
    modulePath.resize(applicationDirectorySeparator + 1);
    modulePath += L"AudioPlaybackConnector2.Control.exe";
    return modulePath;
}

[[noreturn]] void ThrowWin32Error(DWORD error) {
    throw std::system_error(static_cast<int>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error),
                            std::system_category());
}

bool SameRequest(apc::control::Request const& lhs, apc::control::Request const& rhs) noexcept {
    return lhs.Command == rhs.Command && lhs.Target == rhs.Target && lhs.Flags == rhs.Flags &&
           lhs.Payload == rhs.Payload;
}

std::size_t RequestBytes(apc::control::Request const& request) noexcept {
    return sizeof(request) + request.Payload.size() * sizeof(wchar_t);
}

std::size_t ResponseBytes(apc::control::Response const& response) noexcept {
    return sizeof(response) + response.Payload.size() * sizeof(wchar_t);
}

FILETIME RelativeDelay(DWORD delayMilliseconds) noexcept {
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(std::max<DWORD>(1, delayMilliseconds)) * 10'000;
    FILETIME value{};
    value.dwLowDateTime = due.LowPart;
    value.dwHighDateTime = due.HighPart;
    return value;
}

DWORD RetryDelay(DWORD initialDelay, std::size_t failures, std::size_t salt) noexcept {
    constexpr DWORD maximumDelay = 30'000;
    const auto shift = static_cast<unsigned>(std::min<std::size_t>(failures > 0 ? failures - 1 : 0, 7));
    const auto scaled = std::min<std::uint64_t>(maximumDelay, static_cast<std::uint64_t>(initialDelay) << shift);
    const auto jitterRange = std::max<std::uint64_t>(1, scaled / 5);
    const auto jitter = (GetTickCount64() ^ (static_cast<std::uint64_t>(salt) * 0x9E3779B9ull)) % jitterRange;
    return static_cast<DWORD>(std::min<std::uint64_t>(maximumDelay, scaled + jitter));
}

FILETIME AbsoluteDeadline(std::uint64_t deadline) noexcept {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER due{};
    due.LowPart = now.dwLowDateTime;
    due.HighPart = now.dwHighDateTime;
    due.QuadPart += static_cast<ULONGLONG>(std::max<DWORD>(1, apc::control::RemainingWait(deadline))) * 10'000;
    FILETIME value{};
    value.dwLowDateTime = due.LowPart;
    value.dwHighDateTime = due.HighPart;
    return value;
}

} // namespace

struct CommandLineControlServer::RequestRecord {
    apc::control::Request Request;
    apc::control::Response Response;
    std::condition_variable Completed;
    std::chrono::steady_clock::time_point CompletedAt{};
    std::chrono::steady_clock::time_point LastDeliveryCompletedAt{};
    std::size_t Bytes = 0;
    std::size_t ActiveDeliveries = 0;
    bool IsComplete = false;
    bool Acknowledged = false;
};

struct CommandLineControlServer::PipeInstance {
    PipeInstance(CommandLineControlServer* owner, std::size_t index, std::wstring name)
        : Owner(owner), Index(index), Name(std::move(name)) {}
    ~PipeInstance() {
        if (DeadlineTimer) CloseThreadpoolTimer(DeadlineTimer);
        if (RearmTimer) CloseThreadpoolTimer(RearmTimer);
        if (HandlerWork) CloseThreadpoolWork(HandlerWork);
        if (AlreadyConnectedWork) CloseThreadpoolWork(AlreadyConnectedWork);
        if (RecreateWork) CloseThreadpoolWork(RecreateWork);
        if (Pipe && Pipe != INVALID_HANDLE_VALUE) CloseHandle(Pipe);
        if (Io) CloseThreadpoolIo(Io);
    }

    CommandLineControlServer* Owner = nullptr;
    std::size_t Index = 0;
    std::wstring Name;
    HANDLE Pipe = INVALID_HANDLE_VALUE;
    PTP_IO Io = nullptr;
    PTP_WORK AlreadyConnectedWork = nullptr;
    PTP_WORK HandlerWork = nullptr;
    PTP_WORK RecreateWork = nullptr;
    PTP_TIMER DeadlineTimer = nullptr;
    PTP_TIMER RearmTimer = nullptr;
    OVERLAPPED Overlapped{};
    std::mutex StateMutex;

    PipePhase Phase = PipePhase::Disconnected;
    void* Buffer = nullptr;
    std::uint32_t TotalBytes = 0;
    std::uint32_t TransferredBytes = 0;
    bool Write = false;
    std::uint64_t TransferDeadline = 0;
    std::uint64_t RequestDeadline = 0;
    std::uint64_t ResponseDeadline = 0;
    std::size_t RearmFailures = 0;
    bool RecreateRequired = false;
    bool RecreateScheduled = false;
    bool PendingDelivery = false;

    apc::control::RequestHeader RequestHeader;
    apc::control::ResponseHeader ResponseHeader;
    apc::control::Acknowledgement Acknowledgement;
    apc::control::Request Request;
    apc::control::Response Response;
    std::shared_ptr<RequestRecord> AcknowledgementRecord;
};

static_assert(std::is_nothrow_move_assignable_v<apc::control::Response>);

CommandLineControlServer::CommandLineControlServer() {
    const auto expectedIdentity = [] {
        auto path = ExpectedUnpackagedControlPath();
        return path ? apc::control::ExecutableIdentityFromPath(*path) : std::nullopt;
    }();
    m_options.IsTrustedClient = [expectedIdentity](HANDLE pipe) noexcept {
        return apc::control::IsTrustedNamedPipeClient(pipe, expectedIdentity);
    };
}

CommandLineControlServer::CommandLineControlServer(Options options) : m_options(std::move(options)) {
    m_options.PipeInstanceCount =
        std::clamp<std::size_t>(m_options.PipeInstanceCount, 1, apc::control::c_pipeInstanceCount);
    m_options.RequestTimeoutMs = std::max<DWORD>(1, m_options.RequestTimeoutMs);
    m_options.HandlerTimeoutMs = std::max<DWORD>(1, m_options.HandlerTimeoutMs);
    m_options.ResponseTimeoutMs = std::max<DWORD>(1, m_options.ResponseTimeoutMs);
    m_options.AcknowledgementTimeoutMs = std::max<DWORD>(1, m_options.AcknowledgementTimeoutMs);
    m_options.RetryDelayMs = std::max<DWORD>(1, m_options.RetryDelayMs);
    m_options.MaxRequestRecords = std::max<std::size_t>(1, m_options.MaxRequestRecords);
    m_options.MaxRequestCacheBytes = std::max<std::size_t>(
        sizeof(apc::control::Request) + sizeof(apc::control::Response) + 2 * apc::control::c_maxPayloadBytes,
        m_options.MaxRequestCacheBytes);
    m_options.RequestRecordLifetime = std::max(std::chrono::milliseconds(1), m_options.RequestRecordLifetime);
    m_options.AcknowledgedRecordLifetime = std::max(std::chrono::milliseconds(1), m_options.AcknowledgedRecordLifetime);
}

CommandLineControlServer::~CommandLineControlServer() {
    if (g_activeHandlerServer == this) std::terminate();
    Stop();
    if (m_startRetryTimer) {
        SetThreadpoolTimer(m_startRetryTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_startRetryTimer, TRUE);
        CloseThreadpoolTimer(m_startRetryTimer);
        m_startRetryTimer = nullptr;
    }
    if (m_requestPruneTimer) {
        SetThreadpoolTimer(m_requestPruneTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_requestPruneTimer, TRUE);
        CloseThreadpoolTimer(m_requestPruneTimer);
        m_requestPruneTimer = nullptr;
    }
    if (m_deferredStopWork) {
        WaitForThreadpoolWorkCallbacks(m_deferredStopWork, FALSE);
        CloseThreadpoolWork(m_deferredStopWork);
        m_deferredStopWork = nullptr;
    }
}

void CommandLineControlServer::Trace(std::wstring_view message) const noexcept {
    try {
        std::wstring output(L"[AudioPlaybackConnector2.Control] ");
        output.append(message);
        output.push_back(L'\n');
        OutputDebugStringW(output.c_str());
    } catch (...) {
        OutputDebugStringW(L"[AudioPlaybackConnector2.Control] diagnostic callback failed\n");
    }
}

bool CommandLineControlServer::EnsureControlCallbacksLocked() noexcept {
    if (!m_startRetryTimer) {
        m_startRetryTimer = CreateThreadpoolTimer(OnStartRetry, this, nullptr);
        if (!m_startRetryTimer) return false;
    }
    if (!m_requestPruneTimer) {
        m_requestPruneTimer = CreateThreadpoolTimer(OnRequestPrune, this, nullptr);
        if (!m_requestPruneTimer) return false;
    }
    if (!m_deferredStopWork) {
        m_deferredStopWork = CreateThreadpoolWork(OnDeferredStop, this, nullptr);
        if (!m_deferredStopWork) return false;
    }
    return true;
}

void CommandLineControlServer::Start(Handler handler) noexcept {
    if (g_activeHandlerServer == this && m_deferredStopRequested.load()) {
        Trace(L"reentrant restart ignored until deferred stop completes");
        return;
    }
    try {
        {
            std::unique_lock lifecycleLock(m_lifecycleMutex);
            m_lifecycleChanged.wait(lifecycleLock, [this] { return !m_stopping && !m_deferredStopRequested.load(); });
            if (m_running.load() || m_desiredRunning) return;
            if (!EnsureControlCallbacksLocked()) {
                Trace(L"thread-pool control objects unavailable");
                return;
            }
            m_handler = std::move(handler);
            m_desiredRunning = true;
        }
        (void)TryStart();
    } catch (...) {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        m_desiredRunning = false;
        m_handler = nullptr;
        Trace(L"server start request failed");
    }
}

bool CommandLineControlServer::TryStart() noexcept {
    std::unique_lock lifecycleLock(m_lifecycleMutex);
    if (!m_desiredRunning || m_running.load() || m_starting || m_stopping) return m_running.load();
    m_starting = true;

    try {
        auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
        if (!security) ThrowWin32Error(GetLastError());

        std::wstring pipeName;
        if (m_options.PipeName.empty()) {
            auto generatedPipeName = apc::control::PipeName();
            if (!generatedPipeName) ThrowWin32Error(ERROR_NO_SUCH_LOGON_SESSION);
            pipeName = std::move(*generatedPipeName);
        } else {
            pipeName = m_options.PipeName;
        }
        std::vector<std::unique_ptr<PipeInstance>> instances;
        instances.reserve(m_options.PipeInstanceCount);
        std::size_t availableInstances = 0;
        DWORD instanceCreationError = ERROR_SUCCESS;
        for (std::size_t index = 0; index < m_options.PipeInstanceCount; ++index) {
            auto instance =
                std::make_unique<PipeInstance>(this, index, apc::control::PipeInstanceName(pipeName, index));
            instance->Pipe =
                CreateNamedPipeW(instance->Name.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 1,
                                 apc::control::c_pipeBufferBytes,
                                 apc::control::c_pipeBufferBytes,
                                 0,
                                 security->Get());
            if (instance->Pipe != INVALID_HANDLE_VALUE) {
                instance->Io = CreateThreadpoolIo(instance->Pipe, OnIoCompleted, instance.get(), nullptr);
                if (instance->Io) {
                    ++availableInstances;
                } else {
                    instanceCreationError = GetLastError();
                    CloseHandle(std::exchange(instance->Pipe, INVALID_HANDLE_VALUE));
                }
            } else {
                instanceCreationError = GetLastError();
            }
            if (!instance->Io) instance->RecreateRequired = true;
            instance->AlreadyConnectedWork = CreateThreadpoolWork(OnAlreadyConnected, instance.get(), nullptr);
            if (!instance->AlreadyConnectedWork) ThrowWin32Error(GetLastError());
            instance->HandlerWork = CreateThreadpoolWork(OnHandlerReady, instance.get(), nullptr);
            if (!instance->HandlerWork) ThrowWin32Error(GetLastError());
            instance->RecreateWork = CreateThreadpoolWork(OnRecreateReady, instance.get(), nullptr);
            if (!instance->RecreateWork) ThrowWin32Error(GetLastError());
            instance->DeadlineTimer = CreateThreadpoolTimer(OnOperationDeadline, instance.get(), nullptr);
            if (!instance->DeadlineTimer) ThrowWin32Error(GetLastError());
            instance->RearmTimer = CreateThreadpoolTimer(OnRearmReady, instance.get(), nullptr);
            if (!instance->RearmTimer) ThrowWin32Error(GetLastError());
            instances.push_back(std::move(instance));
        }
        if (availableInstances == 0) ThrowWin32Error(instanceCreationError);

        m_stopSource = std::stop_source{};
        m_instances = std::move(instances);
        m_accepting = true;
        m_running = true;
        m_starting = false;
        m_startRetryFailures = 0;
        SetThreadpoolTimer(m_startRetryTimer, nullptr, 0, 0);

        for (auto& instance : m_instances) {
            if (!ArmConnection(*instance)) Trace(L"pipe instance arm deferred");
        }
        Trace(L"server started");
        lifecycleLock.unlock();
        m_lifecycleChanged.notify_all();
        return true;
    } catch (...) {
        m_accepting = false;
        m_running = false;
        m_starting = false;
        m_instances.clear();
        ++m_startRetryFailures;
        Trace(L"server start failed; control endpoint remains optional");
        ScheduleStartRetryLocked();
        lifecycleLock.unlock();
        m_lifecycleChanged.notify_all();
        return false;
    }
}

void CommandLineControlServer::ScheduleStartRetryLocked() noexcept {
    if (!m_desiredRunning || !m_options.RetryStartupFailures || !m_startRetryTimer) return;
    auto due = RelativeDelay(RetryDelay(m_options.RetryDelayMs, m_startRetryFailures, GetCurrentProcessId()));
    SetThreadpoolTimer(m_startRetryTimer, &due, 0, 0);
}

void CommandLineControlServer::Stop() noexcept {
    if (g_activeHandlerServer == this) {
        m_accepting = false;
        m_running = false;
        m_stopSource.request_stop();
        if (!m_deferredStopRequested.exchange(true) && m_deferredStopWork) {
            SubmitThreadpoolWork(m_deferredStopWork);
        }
        return;
    }

    try {
        std::vector<std::unique_ptr<PipeInstance>> instances;
        {
            std::unique_lock lifecycleLock(m_lifecycleMutex);
            if (m_stopping) {
                m_lifecycleChanged.wait(lifecycleLock, [this] { return !m_stopping; });
                return;
            }
            if (!m_desiredRunning && !m_running.load() && !m_starting) return;

            m_desiredRunning = false;
            m_accepting = false;
            m_stopping = true;
            m_running = false;
            m_stopSource.request_stop();
            if (m_startRetryTimer) SetThreadpoolTimer(m_startRetryTimer, nullptr, 0, 0);
            instances.swap(m_instances);
        }

        if (m_startRetryTimer) WaitForThreadpoolTimerCallbacks(m_startRetryTimer, TRUE);

        for (auto& instance : instances) {
            std::scoped_lock stateLock(instance->StateMutex);
            SetThreadpoolTimer(instance->DeadlineTimer, nullptr, 0, 0);
            SetThreadpoolTimer(instance->RearmTimer, nullptr, 0, 0);
            if (instance->Pipe && instance->Pipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(instance->Pipe, &instance->Overlapped);
            }
        }
        for (auto& instance : instances) {
            WaitForThreadpoolTimerCallbacks(instance->DeadlineTimer, TRUE);
            WaitForThreadpoolTimerCallbacks(instance->RearmTimer, TRUE);
            WaitForThreadpoolWorkCallbacks(instance->RecreateWork, TRUE);
            if (instance->Io) WaitForThreadpoolIoCallbacks(instance->Io, FALSE);
            WaitForThreadpoolWorkCallbacks(instance->HandlerWork, TRUE);
            WaitForThreadpoolWorkCallbacks(instance->AlreadyConnectedWork, TRUE);
            if (instance->Pipe && instance->Pipe != INVALID_HANDLE_VALUE) DisconnectNamedPipe(instance->Pipe);
        }
        instances.clear();

        {
            std::lock_guard requestLock(m_requestMutex);
            m_pendingDeliveries.clear();
            for (auto entry = m_requestRecords.begin(); entry != m_requestRecords.end();) {
                if (!entry->second->IsComplete) {
                    m_requestCacheBytes -= std::min(m_requestCacheBytes, entry->second->Bytes);
                    entry = m_requestRecords.erase(entry);
                    continue;
                }
                entry->second->ActiveDeliveries = 0;
                ++entry;
            }
            ScheduleRequestPruneLocked(std::chrono::steady_clock::now());
        }
        {
            std::lock_guard lifecycleLock(m_lifecycleMutex);
            m_handler = nullptr;
            m_starting = false;
            m_stopping = false;
            m_deferredStopRequested = false;
        }
        Trace(L"server stopped");
        m_lifecycleChanged.notify_all();
    } catch (...) {
        {
            std::lock_guard lifecycleLock(m_lifecycleMutex);
            m_desiredRunning = false;
            m_accepting = false;
            m_running = false;
            m_starting = false;
            m_stopping = false;
            m_deferredStopRequested = false;
        }
        m_lifecycleChanged.notify_all();
        Trace(L"server stop encountered an unexpected failure");
    }
}

bool CommandLineControlServer::ArmConnection(PipeInstance& instance) noexcept {
    try {
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
        bool allowArm = true;
        try {
            allowArm = !m_options.BeforeArmConnection || m_options.BeforeArmConnection(instance.Index);
        } catch (...) {
            allowArm = false;
        }
#endif
        std::scoped_lock stateLock(instance.StateMutex);
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
        if (!allowArm) {
            SetLastError(ERROR_RETRY);
            if (instance.RearmFailures >= 7) instance.RecreateRequired = true;
            ScheduleRearmLocked(instance);
            return false;
        }
#endif
        return ArmConnectionLocked(instance);
    } catch (...) {
        Trace(L"pipe arm failed");
        return false;
    }
}

bool CommandLineControlServer::ArmConnectionLocked(PipeInstance& instance) noexcept {
    if (!m_running.load()) return false;
    if (!instance.Io || !instance.Pipe || instance.Pipe == INVALID_HANDLE_VALUE) {
        instance.RecreateRequired = true;
        ScheduleRearmLocked(instance);
        return false;
    }

    instance.Phase = PipePhase::Connecting;
    instance.Overlapped = {};
    StartThreadpoolIo(instance.Io);
    if (ConnectNamedPipe(instance.Pipe, &instance.Overlapped)) return true;

    const auto error = GetLastError();
    if (error == ERROR_IO_PENDING) return true;

    CancelThreadpoolIo(instance.Io);
    if (error == ERROR_PIPE_CONNECTED) {
        SubmitThreadpoolWork(instance.AlreadyConnectedWork);
        return true;
    }
    if (error == ERROR_NO_DATA) {
        DisconnectNamedPipe(instance.Pipe);
        instance.RearmFailures = 0;
    }

    SetLastError(error);
    if (error == ERROR_INVALID_HANDLE || error == ERROR_ACCESS_DENIED || instance.RearmFailures >= 7) {
        instance.RecreateRequired = true;
    }
    ScheduleRearmLocked(instance);
    return false;
}

void CommandLineControlServer::ScheduleRearmLocked(PipeInstance& instance) noexcept {
    instance.Phase = PipePhase::Disconnected;
    if (!m_running.load()) return;
    ++instance.RearmFailures;
    auto due = RelativeDelay(RetryDelay(m_options.RetryDelayMs, instance.RearmFailures, instance.Index));
    SetThreadpoolTimer(instance.RearmTimer, &due, 0, 0);
}

void CommandLineControlServer::RecreatePipeInstance(PipeInstance& instance) noexcept {
    auto failAndRetry = [&]() noexcept {
        try {
            std::scoped_lock stateLock(instance.StateMutex);
            instance.RecreateScheduled = false;
            if (m_running.load()) {
                instance.RecreateRequired = true;
                ScheduleRearmLocked(instance);
            }
        } catch (...) {
            Trace(L"pipe instance recreation retry failed");
        }
    };

    try {
        auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
        if (!security) {
            failAndRetry();
            return;
        }

        HANDLE oldPipe = INVALID_HANDLE_VALUE;
        PTP_IO oldIo = nullptr;
        {
            std::scoped_lock stateLock(instance.StateMutex);
            if (!m_running.load() || !instance.RecreateRequired) {
                instance.RecreateScheduled = false;
                return;
            }
            oldPipe = std::exchange(instance.Pipe, INVALID_HANDLE_VALUE);
            oldIo = std::exchange(instance.Io, nullptr);
        }

        if (oldPipe && oldPipe != INVALID_HANDLE_VALUE) CancelIoEx(oldPipe, nullptr);
        if (oldIo) WaitForThreadpoolIoCallbacks(oldIo, FALSE);
        if (oldPipe && oldPipe != INVALID_HANDLE_VALUE) CloseHandle(oldPipe);
        if (oldIo) CloseThreadpoolIo(oldIo);

        HANDLE newPipe = CreateNamedPipeW(instance.Name.c_str(),
                                          PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                          1,
                                          apc::control::c_pipeBufferBytes,
                                          apc::control::c_pipeBufferBytes,
                                          0,
                                          security->Get());
        if (newPipe == INVALID_HANDLE_VALUE) {
            failAndRetry();
            return;
        }
        auto newIo = CreateThreadpoolIo(newPipe, OnIoCompleted, &instance, nullptr);
        if (!newIo) {
            CloseHandle(newPipe);
            failAndRetry();
            return;
        }

        {
            std::scoped_lock stateLock(instance.StateMutex);
            if (!m_running.load()) {
                CloseHandle(newPipe);
                CloseThreadpoolIo(newIo);
                instance.RecreateScheduled = false;
                return;
            }
            instance.Pipe = newPipe;
            instance.Io = newIo;
            instance.RecreateRequired = false;
            instance.RecreateScheduled = false;
            instance.RearmFailures = 0;
            if (!ArmConnectionLocked(instance)) Trace(L"recreated pipe instance arm deferred");
        }
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
        try {
            if (m_options.AfterPipeRecreated) m_options.AfterPipeRecreated(instance.Index);
        } catch (...) {
        }
#endif
    } catch (...) {
        failAndRetry();
    }
}

bool CommandLineControlServer::StartTransferLocked(
    PipeInstance& instance, void* buffer, std::uint32_t byteCount, bool write, std::uint64_t deadline) noexcept {
    if (!buffer || byteCount == 0 || deadline == 0) return false;
    instance.Buffer = buffer;
    instance.TotalBytes = byteCount;
    instance.TransferredBytes = 0;
    instance.Write = write;
    instance.TransferDeadline = deadline;
    return StartCurrentTransferLocked(instance);
}

bool CommandLineControlServer::StartCurrentTransferLocked(PipeInstance& instance) noexcept {
    if (!m_running.load() || instance.TransferredBytes >= instance.TotalBytes ||
        apc::control::RemainingWait(instance.TransferDeadline) == 0) {
        return false;
    }

    instance.Overlapped = {};
    auto due = AbsoluteDeadline(instance.TransferDeadline);
    SetThreadpoolTimer(instance.DeadlineTimer, &due, 0, 0);
    StartThreadpoolIo(instance.Io);

    auto* cursor = static_cast<std::byte*>(instance.Buffer) + instance.TransferredBytes;
    const DWORD remaining = instance.TotalBytes - instance.TransferredBytes;
    const BOOL started = instance.Write ? WriteFile(instance.Pipe, cursor, remaining, nullptr, &instance.Overlapped)
                                        : ReadFile(instance.Pipe, cursor, remaining, nullptr, &instance.Overlapped);
    if (started || GetLastError() == ERROR_IO_PENDING) return true;

    const auto error = GetLastError();
    CancelThreadpoolIo(instance.Io);
    SetThreadpoolTimer(instance.DeadlineTimer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(instance.DeadlineTimer, TRUE);
    SetLastError(error);
    return false;
}

void CALLBACK CommandLineControlServer::OnIoCompleted(
    PTP_CALLBACK_INSTANCE, void* context, void* overlapped, ULONG ioResult, ULONG_PTR bytes, PTP_IO) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (instance && instance->Owner) instance->Owner->HandleIoCompletion(*instance, overlapped, ioResult, bytes);
}

void CALLBACK CommandLineControlServer::OnAlreadyConnected(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (instance && instance->Owner) instance->Owner->HandleConnectedInstance(*instance);
}

void CALLBACK CommandLineControlServer::OnHandlerReady(PTP_CALLBACK_INSTANCE callbackInstance,
                                                       void* context,
                                                       PTP_WORK) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (!instance || !instance->Owner) return;
    auto* owner = instance->Owner;
    (void)CallbackMayRunLong(callbackInstance);

    apc::control::Request request;
    apc::control::CorrelationId pendingCorrelation{};
    bool canRun = false;
    bool pendingDelivery = false;
    try {
        std::scoped_lock stateLock(instance->StateMutex);
        pendingCorrelation = instance->Request.CorrelationId;
        pendingDelivery = std::exchange(instance->PendingDelivery, false);
        if (owner->m_running.load() && instance->Phase == PipePhase::RunningHandler) {
            request = instance->Request;
            canRun = true;
        }
    } catch (...) {
        owner->Trace(L"handler request copy failed");
    }

    apc::control::Response response{apc::control::ExitCode::OperationFailed, L"", request.CorrelationId};
    std::shared_ptr<RequestRecord> executionRecord;
    std::shared_ptr<RequestRecord> acknowledgementRecord;
    if (canRun) {
        try {
            util::RuntimeApartment apartment;
            if (apartment.Ready()) {
                g_activeHandlerServer = owner;
                executionRecord = owner->ExecuteOnce(request,
                                                     owner->m_stopSource.get_token(),
                                                     apc::control::DeadlineAfter(owner->m_options.HandlerTimeoutMs),
                                                     response);
                g_activeHandlerServer = nullptr;
            }
        } catch (...) {
            g_activeHandlerServer = nullptr;
            owner->Trace(L"handler execution failed");
            if (pendingDelivery) {
                owner->CompletePendingDelivery(pendingCorrelation);
                pendingDelivery = false;
            }
            owner->FinishClient(*instance);
            return;
        }
    }

    auto const& responseToSend = executionRecord ? executionRecord->Response : response;

#ifdef APC_COMMAND_PIPE_SERVER_TESTING
    if (pendingDelivery) {
        try {
            if (owner->m_options.BeforeDeliveryPromoted) owner->m_options.BeforeDeliveryPromoted(instance->Index);
        } catch (...) {
        }
    }
#endif
    if (pendingDelivery) {
        acknowledgementRecord = canRun ? owner->PromotePendingDelivery(request, responseToSend) : nullptr;
        if (!canRun) owner->CompletePendingDelivery(pendingCorrelation);
        pendingDelivery = false;
    }

    try {
        std::scoped_lock stateLock(instance->StateMutex);
        if (owner->m_running.load() && instance->Phase == PipePhase::RunningHandler) {
            instance->Response = responseToSend;
            instance->AcknowledgementRecord = std::move(acknowledgementRecord);
            instance->ResponseHeader = {};
            instance->ResponseHeader.CorrelationHigh = instance->Response.CorrelationId.High;
            instance->ResponseHeader.CorrelationLow = instance->Response.CorrelationId.Low;
            instance->ResponseHeader.ExitCode = static_cast<std::uint32_t>(instance->Response.Code);
            instance->ResponseHeader.PayloadBytes = apc::control::PayloadByteCount(instance->Response.Payload).value();
            instance->ResponseDeadline = apc::control::DeadlineAfter(owner->m_options.ResponseTimeoutMs);
            instance->Phase = PipePhase::WritingResponseHeader;
            if (!owner->StartTransferLocked(*instance,
                                            &instance->ResponseHeader,
                                            sizeof(instance->ResponseHeader),
                                            true,
                                            instance->ResponseDeadline)) {
                owner->FinishClientLocked(*instance);
            }
        }
    } catch (...) {
        owner->Trace(L"handler response dispatch failed");
        owner->FinishClient(*instance);
    }
    if (acknowledgementRecord) {
        owner->CompleteDelivery(request.CorrelationId, acknowledgementRecord, false);
    }
}

void CALLBACK CommandLineControlServer::OnOperationDeadline(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (instance && instance->Pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(instance->Pipe, &instance->Overlapped);
    }
}

void CALLBACK CommandLineControlServer::OnRearmReady(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (!instance || !instance->Owner) return;

    bool recreate = false;
    bool arm = false;
    try {
        std::scoped_lock stateLock(instance->StateMutex);
        if (!instance->Owner->m_running.load()) return;
        if (instance->RecreateRequired) {
            if (!instance->RecreateScheduled) {
                instance->RecreateScheduled = true;
                recreate = true;
            }
        } else {
            arm = true;
        }
    } catch (...) {
        return;
    }
    if (recreate) {
        SubmitThreadpoolWork(instance->RecreateWork);
    } else if (arm) {
        (void)instance->Owner->ArmConnection(*instance);
    }
}

void CALLBACK CommandLineControlServer::OnRecreateReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    auto* instance = static_cast<PipeInstance*>(context);
    if (instance && instance->Owner) instance->Owner->RecreatePipeInstance(*instance);
}

void CALLBACK CommandLineControlServer::OnStartRetry(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    auto* owner = static_cast<CommandLineControlServer*>(context);
    if (owner) (void)owner->TryStart();
}

void CALLBACK CommandLineControlServer::OnRequestPrune(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    auto* owner = static_cast<CommandLineControlServer*>(context);
    if (!owner) return;
    std::size_t recordCount = 0;
    std::size_t cacheBytes = 0;
    try {
        std::lock_guard requestLock(owner->m_requestMutex);
        const auto now = std::chrono::steady_clock::now();
        owner->PruneRequestRecords(now);
        owner->ScheduleRequestPruneLocked(now);
        recordCount = owner->m_requestRecords.size();
        cacheBytes = owner->m_requestCacheBytes;
    } catch (...) {
        owner->Trace(L"request-cache timer failed");
        return;
    }
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
    try {
        if (owner->m_options.AfterRequestCachePruned) {
            owner->m_options.AfterRequestCachePruned(recordCount, cacheBytes);
        }
    } catch (...) {
    }
#endif
    (void)recordCount;
    (void)cacheBytes;
}

void CALLBACK CommandLineControlServer::OnDeferredStop(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    auto* owner = static_cast<CommandLineControlServer*>(context);
    if (owner) owner->Stop();
}

void CommandLineControlServer::HandleIoCompletion(PipeInstance& instance,
                                                  void* overlapped,
                                                  ULONG ioResult,
                                                  ULONG_PTR bytes) noexcept {
    try {
        std::unique_lock stateLock(instance.StateMutex);
        SetThreadpoolTimer(instance.DeadlineTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(instance.DeadlineTimer, TRUE);
        if (overlapped != &instance.Overlapped) {
            FinishClientLocked(instance);
            return;
        }
        if (!m_running.load()) return;
        if (ioResult != ERROR_SUCCESS) {
            FinishClientLocked(instance);
            return;
        }
        if (instance.Phase == PipePhase::Connecting) {
            stateLock.unlock();
            HandleConnectedInstance(instance);
            return;
        }
        if (apc::control::RemainingWait(instance.TransferDeadline) == 0) {
            FinishClientLocked(instance);
            return;
        }
        if (bytes == 0 || bytes > instance.TotalBytes - instance.TransferredBytes) {
            FinishClientLocked(instance);
            return;
        }

        instance.TransferredBytes += static_cast<std::uint32_t>(bytes);
        if (instance.TransferredBytes < instance.TotalBytes) {
            if (!StartCurrentTransferLocked(instance)) FinishClientLocked(instance);
            return;
        }
        HandleCompletedTransferLocked(instance);
    } catch (...) {
        Trace(L"I/O completion failed");
        FinishClient(instance);
    }
}

void CommandLineControlServer::HandleConnectedInstance(PipeInstance& instance) noexcept {
    bool trusted = false;
    try {
        trusted = m_options.IsTrustedClient && m_options.IsTrustedClient(instance.Pipe);
    } catch (...) {
        trusted = false;
    }

    try {
        std::scoped_lock stateLock(instance.StateMutex);
        HandleConnectedInstanceLocked(instance, trusted);
    } catch (...) {
        Trace(L"connected-client setup failed");
        FinishClient(instance);
    }
}

void CommandLineControlServer::HandleConnectedInstanceLocked(PipeInstance& instance, bool trusted) {
    if (!m_running.load() || !m_accepting.load()) {
        FinishClientLocked(instance);
        return;
    }
    if (!trusted) {
        FinishClientLocked(instance);
        return;
    }

    instance.Request = {};
    instance.Response = {};
    instance.AcknowledgementRecord.reset();
    instance.RequestHeader = {};
    instance.RequestDeadline = apc::control::DeadlineAfter(m_options.RequestTimeoutMs);
    instance.RearmFailures = 0;
    instance.Phase = PipePhase::ReadingRequestHeader;
    if (!StartTransferLocked(
            instance, &instance.RequestHeader, sizeof(instance.RequestHeader), false, instance.RequestDeadline)) {
        FinishClientLocked(instance);
    }
}

void CommandLineControlServer::HandleCompletedTransferLocked(PipeInstance& instance) {
    switch (instance.Phase) {
        case PipePhase::ReadingRequestHeader: {
            const auto& header = instance.RequestHeader;
            const apc::control::CorrelationId correlation{header.CorrelationHigh, header.CorrelationLow};
            if (header.Magic != apc::control::c_requestMagic || header.Version != apc::control::c_protocolVersion ||
                correlation.Empty() || !apc::control::IsKnownCommand(header.Command) ||
                !apc::control::IsKnownTarget(header.Target) ||
                (header.Flags & ~(apc::control::CommandFlagJson | apc::control::CommandFlagRaw)) != 0 ||
                !apc::control::IsPayloadByteCountValid(header.PayloadBytes)) {
                FinishClientLocked(instance);
                return;
            }

            instance.Request.Command = static_cast<apc::control::CommandType>(header.Command);
            instance.Request.Target = static_cast<apc::control::TargetKind>(header.Target);
            instance.Request.Flags = header.Flags;
            instance.Request.CorrelationId = correlation;
            instance.Request.Payload.assign(header.PayloadBytes / sizeof(wchar_t), L'\0');
            if (header.PayloadBytes == 0) {
                DispatchRequestLocked(instance);
                return;
            }

            instance.Phase = PipePhase::ReadingRequestPayload;
            if (!StartTransferLocked(
                    instance, instance.Request.Payload.data(), header.PayloadBytes, false, instance.RequestDeadline)) {
                FinishClientLocked(instance);
            }
            return;
        }
        case PipePhase::ReadingRequestPayload: DispatchRequestLocked(instance); return;
        case PipePhase::WritingResponseHeader: {
            if (instance.ResponseHeader.PayloadBytes != 0) {
                instance.Phase = PipePhase::WritingResponsePayload;
                if (!StartTransferLocked(instance,
                                         instance.Response.Payload.data(),
                                         instance.ResponseHeader.PayloadBytes,
                                         true,
                                         instance.ResponseDeadline)) {
                    FinishClientLocked(instance);
                }
                return;
            }
            instance.Acknowledgement = {};
            instance.Phase = PipePhase::ReadingAcknowledgement;
            if (!StartTransferLocked(instance,
                                     &instance.Acknowledgement,
                                     sizeof(instance.Acknowledgement),
                                     false,
                                     apc::control::DeadlineAfter(m_options.AcknowledgementTimeoutMs))) {
                FinishClientLocked(instance);
            }
            return;
        }
        case PipePhase::WritingResponsePayload:
            instance.Acknowledgement = {};
            instance.Phase = PipePhase::ReadingAcknowledgement;
            if (!StartTransferLocked(instance,
                                     &instance.Acknowledgement,
                                     sizeof(instance.Acknowledgement),
                                     false,
                                     apc::control::DeadlineAfter(m_options.AcknowledgementTimeoutMs))) {
                FinishClientLocked(instance);
            }
            return;
        case PipePhase::ReadingAcknowledgement: {
            const auto& acknowledgement = instance.Acknowledgement;
            if (acknowledgement.Magic == apc::control::c_acknowledgementMagic &&
                acknowledgement.Version == apc::control::c_protocolVersion &&
                acknowledgement.CorrelationHigh == instance.Request.CorrelationId.High &&
                acknowledgement.CorrelationLow == instance.Request.CorrelationId.Low &&
                instance.AcknowledgementRecord) {
                auto record = std::move(instance.AcknowledgementRecord);
                CompleteDelivery(instance.Request.CorrelationId, record, true);
            }
            FinishClientLocked(instance);
            return;
        }
        default: FinishClientLocked(instance); return;
    }
}

void CommandLineControlServer::DispatchRequestLocked(PipeInstance& instance) noexcept {
    if (!apc::control::IsRequestValid(instance.Request)) {
        FinishClientLocked(instance);
        return;
    }
    try {
        std::lock_guard requestLock(m_requestMutex);
        auto entry = m_pendingDeliveries.try_emplace(instance.Request.CorrelationId, 0).first;
        if (entry->second == std::numeric_limits<std::size_t>::max()) {
            FinishClientLocked(instance);
            return;
        }
        ++entry->second;
        instance.PendingDelivery = true;
        instance.Phase = PipePhase::RunningHandler;
        SubmitThreadpoolWork(instance.HandlerWork);
    } catch (...) {
        FinishClientLocked(instance);
    }
}

void CommandLineControlServer::FinishClient(PipeInstance& instance) noexcept {
    try {
        std::scoped_lock stateLock(instance.StateMutex);
        FinishClientLocked(instance);
    } catch (...) {
        Trace(L"client cleanup failed");
    }
}

void CommandLineControlServer::FinishClientLocked(PipeInstance& instance) noexcept {
    instance.Phase = PipePhase::Disconnected;
    if (instance.PendingDelivery) {
        instance.PendingDelivery = false;
        CompletePendingDelivery(instance.Request.CorrelationId);
    }
    if (instance.AcknowledgementRecord) {
        auto record = std::move(instance.AcknowledgementRecord);
        CompleteDelivery(instance.Request.CorrelationId, record, false);
    }
    DisconnectNamedPipe(instance.Pipe);
    if (m_running.load() && !ArmConnectionLocked(instance)) Trace(L"pipe rearm deferred");
}

std::shared_ptr<CommandLineControlServer::RequestRecord>
CommandLineControlServer::ExecuteOnce(apc::control::Request const& request,
                                      std::stop_token stopToken,
                                      std::uint64_t deadline,
                                      apc::control::Response& uncachedResponse) {
    const auto now = std::chrono::steady_clock::now();
    const auto reservedBytes = RequestBytes(request) + sizeof(apc::control::Response) + apc::control::c_maxPayloadBytes;
    std::shared_ptr<RequestRecord> record;

    {
        std::unique_lock requestLock(m_requestMutex);
        PruneRequestRecords(now);
        if (auto existing = m_requestRecords.find(request.CorrelationId); existing != m_requestRecords.end()) {
            record = existing->second;
            if (!SameRequest(record->Request, request)) {
                uncachedResponse = {apc::control::ExitCode::InvalidRequest, L"", request.CorrelationId};
                return {};
            }
            while (!record->IsComplete) {
                if (stopToken.stop_requested()) {
                    uncachedResponse = {apc::control::ExitCode::Unavailable, L"", request.CorrelationId};
                    return {};
                }
                const auto wait = apc::control::RemainingWait(deadline);
                if (wait == 0) {
                    uncachedResponse = {apc::control::ExitCode::Busy, L"", request.CorrelationId};
                    return {};
                }
                record->Completed.wait_for(requestLock, std::chrono::milliseconds(std::min<DWORD>(wait, 100)));
            }
            return record;
        }

        const auto lacksCapacity = [&]() noexcept {
            return reservedBytes > m_options.MaxRequestCacheBytes ||
                   m_requestRecords.size() >= m_options.MaxRequestRecords ||
                   m_requestCacheBytes > m_options.MaxRequestCacheBytes - reservedBytes;
        };
        while (lacksCapacity()) {
            auto candidate = m_requestRecords.end();
            for (auto entry = m_requestRecords.begin(); entry != m_requestRecords.end(); ++entry) {
                auto const& current = entry->second;
                const auto pending = m_pendingDeliveries.find(entry->first);
                if (!current->IsComplete || !current->Acknowledged || current->ActiveDeliveries != 0 ||
                    (pending != m_pendingDeliveries.end() && pending->second != 0)) {
                    continue;
                }
                if (candidate == m_requestRecords.end() ||
                    current->LastDeliveryCompletedAt < candidate->second->LastDeliveryCompletedAt) {
                    candidate = entry;
                }
            }
            if (candidate == m_requestRecords.end()) {
                uncachedResponse = {apc::control::ExitCode::Busy, L"", request.CorrelationId};
                return {};
            }
            m_requestCacheBytes -= std::min(m_requestCacheBytes, candidate->second->Bytes);
            m_requestRecords.erase(candidate);
        }

        record = std::make_shared<RequestRecord>();
        record->Request = request;
        record->Bytes = reservedBytes;
        const auto [entry, inserted] = m_requestRecords.emplace(request.CorrelationId, record);
        if (!inserted) {
            uncachedResponse = {apc::control::ExitCode::Busy, L"", request.CorrelationId};
            return {};
        }
        m_requestCacheBytes += reservedBytes;
    }

    apc::control::Response response;
    try {
        response = m_handler ? m_handler(request, stopToken, deadline)
                             : apc::control::Response{apc::control::ExitCode::Unavailable, L""};
    } catch (...) {
        response.Code = apc::control::ExitCode::Indeterminate;
        response.Payload.clear();
    }
    response.CorrelationId = request.CorrelationId;
    if (!apc::control::PayloadByteCount(response.Payload) ||
        !apc::control::IsKnownExitCode(static_cast<std::uint32_t>(response.Code))) {
        response.Code = apc::control::ExitCode::Indeterminate;
        response.Payload = std::wstring{};
    }

    {
        std::lock_guard requestLock(m_requestMutex);
        record->Response = std::move(response);
        record->CompletedAt = std::chrono::steady_clock::now();
        record->IsComplete = true;
        const auto actualBytes = RequestBytes(record->Request) + ResponseBytes(record->Response);
        if (record->Bytes > actualBytes) m_requestCacheBytes -= record->Bytes - actualBytes;
        record->Bytes = actualBytes;
        PruneRequestRecords(record->CompletedAt);
        ScheduleRequestPruneLocked(record->CompletedAt);
    }
    record->Completed.notify_all();
    return record;
}

void CommandLineControlServer::CompleteDelivery(apc::control::CorrelationId correlationId,
                                                std::shared_ptr<RequestRecord> const& record,
                                                bool acknowledged) noexcept {
    try {
        {
            std::lock_guard requestLock(m_requestMutex);
            const auto entry = m_requestRecords.find(correlationId);
            if (entry == m_requestRecords.end() || entry->second != record || !record->IsComplete) return;
            if (record->ActiveDeliveries > 0) --record->ActiveDeliveries;
            record->Acknowledged = record->Acknowledged || acknowledged;
            record->LastDeliveryCompletedAt = std::chrono::steady_clock::now();
            PruneRequestRecords(record->LastDeliveryCompletedAt);
            ScheduleRequestPruneLocked(record->LastDeliveryCompletedAt);
        }
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
        if (m_options.AfterDeliveryCompleted) {
            m_options.AfterDeliveryCompleted(correlationId, acknowledged);
        }
#endif
    } catch (...) {
        Trace(L"request acknowledgement cleanup failed");
    }
}

std::shared_ptr<CommandLineControlServer::RequestRecord>
CommandLineControlServer::PromotePendingDelivery(apc::control::Request const& request,
                                                 apc::control::Response const& response) noexcept {
    try {
        std::lock_guard requestLock(m_requestMutex);
        auto pending = m_pendingDeliveries.find(request.CorrelationId);
        if (pending != m_pendingDeliveries.end()) {
            if (pending->second > 1) {
                --pending->second;
            } else {
                m_pendingDeliveries.erase(pending);
            }
        }
        auto entry = m_requestRecords.find(request.CorrelationId);
        if (entry == m_requestRecords.end() || !entry->second->IsComplete ||
            !SameRequest(entry->second->Request, request) || entry->second->Response.Code != response.Code ||
            entry->second->Response.Payload != response.Payload) {
            return {};
        }
        ++entry->second->ActiveDeliveries;
        return entry->second;
    } catch (...) {
        Trace(L"pending delivery promotion failed");
        return {};
    }
}

void CommandLineControlServer::CompletePendingDelivery(apc::control::CorrelationId correlationId) noexcept {
    try {
        std::lock_guard requestLock(m_requestMutex);
        auto pending = m_pendingDeliveries.find(correlationId);
        if (pending == m_pendingDeliveries.end()) return;
        if (pending->second > 1) {
            --pending->second;
        } else {
            m_pendingDeliveries.erase(pending);
        }
        ScheduleRequestPruneLocked(std::chrono::steady_clock::now());
    } catch (...) {
        Trace(L"pending delivery cleanup failed");
    }
}

void CommandLineControlServer::PruneRequestRecords(std::chrono::steady_clock::time_point now) noexcept {
    for (auto entry = m_requestRecords.begin(); entry != m_requestRecords.end();) {
        auto const& record = entry->second;
        const auto pending = m_pendingDeliveries.find(entry->first);
        const auto retention =
            record->Acknowledged ? m_options.AcknowledgedRecordLifetime : m_options.RequestRecordLifetime;
        const auto retainedSince = record->Acknowledged ? record->LastDeliveryCompletedAt : record->CompletedAt;
        if (!record->IsComplete || record->ActiveDeliveries != 0 ||
            (pending != m_pendingDeliveries.end() && pending->second != 0) || now - retainedSince < retention) {
            ++entry;
            continue;
        }
        m_requestCacheBytes -= std::min(m_requestCacheBytes, record->Bytes);
        entry = m_requestRecords.erase(entry);
    }
}

void CommandLineControlServer::ScheduleRequestPruneLocked(std::chrono::steady_clock::time_point now) noexcept {
    if (!m_requestPruneTimer) return;
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (auto const& [correlationId, record] : m_requestRecords) {
        const auto pending = m_pendingDeliveries.find(correlationId);
        if (!record->IsComplete || record->ActiveDeliveries != 0 ||
            (pending != m_pendingDeliveries.end() && pending->second != 0)) {
            continue;
        }
        const auto retention =
            record->Acknowledged ? m_options.AcknowledgedRecordLifetime : m_options.RequestRecordLifetime;
        const auto retainedSince = record->Acknowledged ? record->LastDeliveryCompletedAt : record->CompletedAt;
        const auto expires = retainedSince + retention;
        if (!earliest || expires < *earliest) earliest = expires;
    }
    if (!earliest) {
        SetThreadpoolTimer(m_requestPruneTimer, nullptr, 0, 0);
        return;
    }
    const auto remaining =
        *earliest > now ? std::chrono::duration_cast<std::chrono::milliseconds>(*earliest - now).count() : 1;
    auto due = RelativeDelay(static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, MAXDWORD - 1)));
    SetThreadpoolTimer(m_requestPruneTimer, &due, 0, 0);
}
