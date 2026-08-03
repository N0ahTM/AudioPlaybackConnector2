#pragma once

#include <windows.h>

#include <control/CommandProtocol.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class CommandLineControlServer {
public:
    using Handler = std::function<apc::control::Response(apc::control::Request const&, std::stop_token, std::uint64_t)>;
    using TrustClient = std::function<bool(HANDLE)>;

    struct Options {
        std::wstring PipeName;
        std::size_t PipeInstanceCount = apc::control::c_pipeInstanceCount;
        DWORD RequestTimeoutMs = 5000;
        DWORD HandlerTimeoutMs = 30000;
        DWORD ResponseTimeoutMs = 5000;
        DWORD AcknowledgementTimeoutMs = 1500;
        DWORD RetryDelayMs = 250;
        std::size_t MaxRequestRecords = 64;
        std::size_t MaxRequestCacheBytes = 1024 * 1024;
        std::chrono::milliseconds RequestRecordLifetime = std::chrono::seconds(90);
        std::chrono::milliseconds AcknowledgedRecordLifetime = std::chrono::seconds(1);
        bool RetryStartupFailures = true;
        TrustClient IsTrustedClient;
#ifdef APC_COMMAND_PIPE_SERVER_TESTING
        std::function<bool(std::size_t)> BeforeArmConnection;
        std::function<void(std::size_t)> AfterPipeRecreated;
        std::function<void(std::size_t)> BeforeDeliveryPromoted;
        std::function<void(apc::control::CorrelationId, bool)> AfterDeliveryCompleted;
        std::function<void(std::size_t, std::size_t)> AfterRequestCachePruned;
#endif
    };

    CommandLineControlServer();
    explicit CommandLineControlServer(Options options);
    ~CommandLineControlServer();

    CommandLineControlServer(CommandLineControlServer const&) = delete;
    CommandLineControlServer& operator=(CommandLineControlServer const&) = delete;

    void Start(Handler handler) noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(); }

private:
    struct PipeInstance;
    struct RequestRecord;

    static void CALLBACK
    OnIoCompleted(PTP_CALLBACK_INSTANCE, void* context, void* overlapped, ULONG ioResult, ULONG_PTR, PTP_IO) noexcept;
    static void CALLBACK OnAlreadyConnected(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept;
    static void CALLBACK OnHandlerReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept;
    static void CALLBACK OnOperationDeadline(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept;
    static void CALLBACK OnRearmReady(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept;
    static void CALLBACK OnRecreateReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept;
    static void CALLBACK OnStartRetry(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept;
    static void CALLBACK OnRequestPrune(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept;
    static void CALLBACK OnDeferredStop(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept;

    bool TryStart() noexcept;
    bool EnsureControlCallbacksLocked() noexcept;
    void ScheduleStartRetryLocked() noexcept;
    void Trace(std::wstring_view message) const noexcept;

    bool ArmConnection(PipeInstance& instance) noexcept;
    bool ArmConnectionLocked(PipeInstance& instance) noexcept;
    void ScheduleRearmLocked(PipeInstance& instance) noexcept;
    void RecreatePipeInstance(PipeInstance& instance) noexcept;
    bool StartTransferLocked(
        PipeInstance& instance, void* buffer, std::uint32_t byteCount, bool write, std::uint64_t deadline) noexcept;
    bool StartCurrentTransferLocked(PipeInstance& instance) noexcept;
    void HandleIoCompletion(PipeInstance& instance, void* overlapped, ULONG ioResult, ULONG_PTR bytes) noexcept;
    void HandleConnectedInstance(PipeInstance& instance) noexcept;
    void HandleConnectedInstanceLocked(PipeInstance& instance, bool trusted);
    void HandleCompletedTransferLocked(PipeInstance& instance);
    void DispatchRequestLocked(PipeInstance& instance) noexcept;
    void FinishClient(PipeInstance& instance) noexcept;
    void FinishClientLocked(PipeInstance& instance) noexcept;
    [[nodiscard]] std::shared_ptr<RequestRecord> ExecuteOnce(apc::control::Request const& request,
                                                             std::stop_token stopToken,
                                                             std::uint64_t deadline,
                                                             apc::control::Response& uncachedResponse);
    void CompleteDelivery(apc::control::CorrelationId correlationId,
                          std::shared_ptr<RequestRecord> const& record,
                          bool acknowledged) noexcept;
    [[nodiscard]] std::shared_ptr<RequestRecord>
    PromotePendingDelivery(apc::control::Request const& request, apc::control::Response const& response) noexcept;
    void CompletePendingDelivery(apc::control::CorrelationId correlationId) noexcept;
    void PruneRequestRecords(std::chrono::steady_clock::time_point now) noexcept;
    void ScheduleRequestPruneLocked(std::chrono::steady_clock::time_point now) noexcept;

    Options m_options;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_accepting = false;
    std::atomic<bool> m_deferredStopRequested = false;
    std::mutex m_lifecycleMutex;
    std::condition_variable m_lifecycleChanged;
    bool m_desiredRunning = false;
    bool m_starting = false;
    bool m_stopping = false;
    std::stop_source m_stopSource;
    Handler m_handler;
    std::vector<std::unique_ptr<PipeInstance>> m_instances;
    PTP_TIMER m_startRetryTimer = nullptr;
    PTP_TIMER m_requestPruneTimer = nullptr;
    PTP_WORK m_deferredStopWork = nullptr;
    std::size_t m_startRetryFailures = 0;

    std::mutex m_requestMutex;
    std::unordered_map<apc::control::CorrelationId, std::shared_ptr<RequestRecord>, apc::control::CorrelationIdHash>
        m_requestRecords;
    std::unordered_map<apc::control::CorrelationId, std::size_t, apc::control::CorrelationIdHash> m_pendingDeliveries;
    std::size_t m_requestCacheBytes = 0;
};
