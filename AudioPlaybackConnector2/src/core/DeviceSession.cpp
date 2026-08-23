#include <pch.h>

#include <core/AudioConnectionService.hpp>
#include <core/DeviceSession.hpp>
#include <core/ReconnectPolicy.hpp>

#include <stdexcept>
#include <utility>

namespace apc::device {
namespace {

DeviceConnectionResult
ToConnectionResult(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus status) noexcept {
    using Status = winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
    switch (status) {
        case Status::Success: return DeviceConnectionResult::Success;
        case Status::RequestTimedOut: return DeviceConnectionResult::TimedOut;
        case Status::DeniedBySystem: return DeviceConnectionResult::Denied;
        default: return DeviceConnectionResult::Failed;
    }
}

class WindowsDeviceConnection final : public DeviceConnection {
public:
    explicit WindowsDeviceConnection(winrt::Windows::Media::Audio::AudioPlaybackConnection connection)
        : m_connection(std::move(connection)) {}

    [[nodiscard]] std::uint64_t RegisterStateChanged(StateChangedHandler handler) override {
        if (!m_connection) return 0;
        m_stateChangedToken = AudioConnectionService::RegisterStateChanged(
            m_connection, [handler = std::move(handler)](auto sender, auto const&) {
                if (!handler) return;
                try {
                    auto const state = sender.State();
                    if (state == winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Opened) {
                        handler(DeviceConnectionState::Opened);
                    } else if (state == winrt::Windows::Media::Audio::AudioPlaybackConnectionState::Closed) {
                        handler(DeviceConnectionState::Closed);
                    }
                } catch (...) {
                    handler(DeviceConnectionState::Closed);
                }
            });
        return static_cast<std::uint64_t>(m_stateChangedToken.value);
    }

    void RevokeStateChanged(std::uint64_t token) noexcept override {
        if (token == 0 || token != static_cast<std::uint64_t>(m_stateChangedToken.value)) return;
        AudioConnectionService::RevokeStateChanged(m_connection, m_stateChangedToken);
        m_stateChangedToken = {};
    }

    void Start(Completion completion) override { StartDetached(m_connection, std::move(completion)); }

    void Open(Completion completion) override { OpenDetached(m_connection, std::move(completion)); }

    void Close(CloseCompletion completion) noexcept override {
        RevokeStateChanged(static_cast<std::uint64_t>(m_stateChangedToken.value));
        auto connection = std::move(m_connection);
        CloseDetached(std::move(connection), std::move(completion));
    }

private:
    static winrt::fire_and_forget StartDetached(winrt::Windows::Media::Audio::AudioPlaybackConnection connection,
                                                Completion completion) {
        try {
            co_await winrt::resume_background();
            co_await AudioConnectionService::StartAsync(connection);
            if (completion) completion(DeviceConnectionResult::Success);
        } catch (winrt::hresult_error const&) {
            if (completion) completion(DeviceConnectionResult::Failed);
        } catch (...) {
            if (completion) completion(DeviceConnectionResult::Failed);
        }
    }

    static winrt::fire_and_forget OpenDetached(winrt::Windows::Media::Audio::AudioPlaybackConnection connection,
                                               Completion completion) {
        try {
            co_await winrt::resume_background();
            auto const result = co_await AudioConnectionService::OpenAsync(connection);
            if (completion) completion(ToConnectionResult(result.Status()));
        } catch (winrt::hresult_error const&) {
            if (completion) completion(DeviceConnectionResult::Failed);
        } catch (...) {
            if (completion) completion(DeviceConnectionResult::Failed);
        }
    }

    static winrt::fire_and_forget CloseDetached(winrt::Windows::Media::Audio::AudioPlaybackConnection connection,
                                                CloseCompletion completion) noexcept {
        try {
            co_await winrt::resume_background();
            AudioConnectionService::Close(connection);
        } catch (...) {
        }
        if (completion) completion();
    }

    winrt::Windows::Media::Audio::AudioPlaybackConnection m_connection{nullptr};
    winrt::event_token m_stateChangedToken{};
};

class WindowsDeviceConnectionPlatform final : public DeviceConnectionPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceConnection> Create(std::wstring const& deviceId) override {
        auto connection = AudioConnectionService::TryCreateFromId(winrt::hstring(deviceId));
        if (!connection) return {};
        return std::make_unique<WindowsDeviceConnection>(std::move(connection));
    }
};

class WindowsDeviceTimer final : public DeviceTimer {
public:
    explicit WindowsDeviceTimer(winrt::Windows::System::Threading::ThreadPoolTimer timer) : m_timer(std::move(timer)) {}
    void Cancel() noexcept override {
        try {
            if (m_timer) m_timer.Cancel();
        } catch (...) {
        }
        m_timer = nullptr;
    }

private:
    winrt::Windows::System::Threading::ThreadPoolTimer m_timer{nullptr};
};

class WindowsDeviceTimerPlatform final : public DeviceTimerPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceTimer> Schedule(std::chrono::milliseconds delay, Callback callback) override {
        auto timer = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [callback = std::move(callback)](auto const&) {
                if (callback) callback();
            },
            delay);
        return std::make_unique<WindowsDeviceTimer>(std::move(timer));
    }
};

} // namespace

std::unique_ptr<DeviceConnectionPlatform> CreateWindowsDeviceConnectionPlatform() {
    return std::make_unique<WindowsDeviceConnectionPlatform>();
}

std::unique_ptr<DeviceTimerPlatform> CreateWindowsDeviceTimerPlatform() {
    return std::make_unique<WindowsDeviceTimerPlatform>();
}

struct DeviceSession::State : std::enable_shared_from_this<DeviceSession::State> {
    enum class CloseContinuation { Idle, Replace, Retry, Suspend };

    static constexpr auto CloseBarrierTimeout = ReconnectPolicy::BlockedRetryDelay;
    static constexpr std::chrono::milliseconds CloseBarrierCooldown{1500};

    State(std::wstring deviceId,
          std::wstring deviceName,
          SerializedExecutor executor,
          DeviceConnectionPlatform& connectionPlatform,
          DeviceTimerPlatform& timerPlatform,
          FactSink factSink)
        : DeviceId(std::move(deviceId)), DeviceName(std::move(deviceName)), Executor(std::move(executor)),
          ConnectionPlatform(connectionPlatform), TimerPlatform(timerPlatform), PublishFact(std::move(factSink)) {}

    std::wstring DeviceId;
    std::wstring DeviceName;
    SerializedExecutor Executor;
    DeviceConnectionPlatform& ConnectionPlatform;
    DeviceTimerPlatform& TimerPlatform;
    FactSink PublishFact;
    std::unique_ptr<DeviceConnection> Connection;
    std::unique_ptr<DeviceConnection> ClosingConnection;
    std::unique_ptr<DeviceTimer> ReconnectTimer;
    std::unique_ptr<DeviceTimer> CloseBarrierTimer;
    std::uint64_t StateChangedToken = 0;
    std::uint64_t OperationEpoch = 0;
    std::uint64_t TimerEpoch = 0;
    std::uint64_t CloseBarrierEpoch = 0;
    std::size_t CompletedRetryAttempts = 0;
    DeviceLifecycleState Lifecycle = DeviceLifecycleState::Idle;
    DeviceOperationKind CurrentOperation = DeviceOperationKind::ManualConnect;
    bool IsIncomingEnabled = false;
    bool IsReconnectEnabled = true;
    bool IsReconnectCancelled = false;
    bool IsSuspended = false;
    bool IsShutdown = false;
    bool OpenImmediately = true;
    bool RetryEligible = false;
    bool RestoreIncomingAfterClose = false;
    bool ResumeRequired = false;
    bool ResumeOpenImmediately = true;
    bool ResumeRequested = false;
    bool IsSuspendClosePending = false;
    std::uint64_t SuspendCloseEpoch = 0;
    bool IsCloseInFlight = false;
    bool IsCloseCooldown = false;
    bool IsCloseContinuationAbandoned = false;
    CloseContinuation PendingCloseContinuation = CloseContinuation::Idle;
    DeviceConnectionResult PendingCloseFailure = DeviceConnectionResult::Success;
    bool PendingCloseRetryEligible = false;
    bool PendingCloseCompletedAttempt = false;

    [[nodiscard]] DeviceSessionSnapshot Snapshot() const {
        return {
            .DeviceId = DeviceId,
            .DeviceName = DeviceName,
            .State = Lifecycle,
            .OperationEpoch = OperationEpoch,
            .CompletedRetryAttempts = CompletedRetryAttempts,
            .HasConnection = static_cast<bool>(Connection),
            .IsIncomingEnabled = IsIncomingEnabled,
            .IsReconnectEnabled = IsReconnectEnabled,
            .IsReconnectCancelled = IsReconnectCancelled,
        };
    }

    [[nodiscard]] bool IsBusy() const noexcept {
        return IsCloseInFlight || Lifecycle == DeviceLifecycleState::Connecting ||
               Lifecycle == DeviceLifecycleState::Disconnecting ||
               Lifecycle == DeviceLifecycleState::WaitingForReconnect;
    }

    void Publish(DeviceConnectionResult result = DeviceConnectionResult::Success, bool terminal = false) {
        if (PublishFact)
            PublishFact({.Snapshot = Snapshot(),
                         .Operation = CurrentOperation,
                         .Result = result,
                         .IsTerminalFailure = terminal});
    }

    void Post(Task task) {
        auto weak = weak_from_this();
        Executor([weak, task = std::move(task)]() mutable {
            if (auto state = weak.lock()) task();
        });
    }

    void CancelTimer() noexcept {
        ++TimerEpoch;
        auto timer = std::move(ReconnectTimer);
        if (timer) timer->Cancel();
    }

    void CancelCloseBarrierTimer() noexcept {
        auto timer = std::move(CloseBarrierTimer);
        if (timer) timer->Cancel();
    }

    void RevokeStateChanged() noexcept {
        if (Connection && StateChangedToken != 0) Connection->RevokeStateChanged(std::exchange(StateChangedToken, 0));
    }

    void StartConnection(DeviceOperationKind operation, bool openImmediately) {
        if (IsShutdown || IsSuspended || DeviceId.empty()) return;
        if (IsCloseInFlight || Lifecycle == DeviceLifecycleState::Connecting ||
            Lifecycle == DeviceLifecycleState::Disconnecting) {
            return;
        }

        if (operation != DeviceOperationKind::AutomaticReconnect) {
            IsReconnectCancelled = false;
            CompletedRetryAttempts = 0;
        }
        CancelTimer();
        CurrentOperation = operation;
        RetryEligible = operation == DeviceOperationKind::AutomaticReconnect ||
                        operation == DeviceOperationKind::Startup || operation == DeviceOperationKind::Resume;
        OpenImmediately = openImmediately;
        ++OperationEpoch;
        auto const epoch = OperationEpoch;

        if (Connection) {
            Lifecycle = DeviceLifecycleState::Disconnecting;
            RestoreIncomingAfterClose = false;
            RevokeStateChanged();
            Publish();
            BeginClose(epoch, CloseContinuation::Replace);
            return;
        }
        BeginCreate(epoch);
    }

    void BeginCreate(std::uint64_t epoch) {
        if (!IsCurrent(epoch) || IsShutdown || IsSuspended) return;
        Lifecycle = DeviceLifecycleState::Connecting;
        Publish();
        try {
            Connection = ConnectionPlatform.Create(DeviceId);
        } catch (...) {
            Connection.reset();
        }
        if (!Connection) {
            CompleteFailure(epoch, DeviceConnectionResult::Failed);
            return;
        }

        auto* const expectedConnection = Connection.get();
        auto weak = weak_from_this();
        try {
            StateChangedToken =
                Connection->RegisterStateChanged([weak, epoch, expectedConnection](DeviceConnectionState state) {
                    if (auto session = weak.lock()) {
                        try {
                            session->Post([weak, epoch, expectedConnection, state] {
                                if (auto current = weak.lock())
                                    current->OnStateChanged(epoch, expectedConnection, state);
                            });
                        } catch (...) {
                            session->PostFailure(epoch, expectedConnection);
                        }
                    }
                });
            if (StateChangedToken == 0) throw std::runtime_error("DeviceSession state handler registration failed");
            Connection->Start([weak, epoch, expectedConnection](DeviceConnectionResult result) {
                if (auto session = weak.lock()) {
                    try {
                        session->Post([weak, epoch, expectedConnection, result] {
                            if (auto current = weak.lock()) current->OnStarted(epoch, expectedConnection, result);
                        });
                    } catch (...) {
                        session->PostFailure(epoch, expectedConnection);
                    }
                }
            });
        } catch (...) {
            CompleteFailure(epoch, DeviceConnectionResult::Failed);
        }
    }

    void OnStarted(std::uint64_t epoch, DeviceConnection const* expectedConnection, DeviceConnectionResult result) {
        if (!IsCurrentConnection(epoch, expectedConnection)) return;
        if (Lifecycle == DeviceLifecycleState::Connected) return;
        if (result != DeviceConnectionResult::Success) {
            CompleteFailure(epoch, result);
            return;
        }
        if (!OpenImmediately) {
            Lifecycle = DeviceLifecycleState::Idle;
            Publish();
            return;
        }
        auto weak = weak_from_this();
        try {
            Connection->Open([weak, epoch, expectedConnection](DeviceConnectionResult openResult) {
                if (auto session = weak.lock()) {
                    try {
                        session->Post([weak, epoch, expectedConnection, openResult] {
                            if (auto current = weak.lock()) current->OnOpened(epoch, expectedConnection, openResult);
                        });
                    } catch (...) {
                        session->PostFailure(epoch, expectedConnection);
                    }
                }
            });
        } catch (...) {
            CompleteFailure(epoch, DeviceConnectionResult::Failed);
        }
    }

    void OnOpened(std::uint64_t epoch, DeviceConnection const* expectedConnection, DeviceConnectionResult result) {
        if (!IsCurrentConnection(epoch, expectedConnection)) return;
        if (Lifecycle == DeviceLifecycleState::Connected) return;
        if (result != DeviceConnectionResult::Success) {
            CompleteFailure(epoch, result);
            return;
        }
        Lifecycle = DeviceLifecycleState::Connected;
        CompletedRetryAttempts = 0;
        RetryEligible = IsReconnectEnabled;
        Publish();
    }

    void OnStateChanged(std::uint64_t epoch, DeviceConnection const* expectedConnection, DeviceConnectionState state) {
        if (!IsCurrentConnection(epoch, expectedConnection) || IsShutdown) return;
        bool const isIncomingListener = !OpenImmediately && IsIncomingEnabled;
        if (state == DeviceConnectionState::Opened) {
            CancelTimer();
            Lifecycle = DeviceLifecycleState::Connected;
            CompletedRetryAttempts = 0;
            RetryEligible = IsReconnectEnabled;
            Publish();
            return;
        }
        if (Lifecycle == DeviceLifecycleState::Disconnecting) return;
        if (isIncomingListener && Lifecycle == DeviceLifecycleState::WaitingForReconnect) return;
        bool const wasEstablished = Lifecycle == DeviceLifecycleState::Connected;
        if (wasEstablished && isIncomingListener) {
            if (!IsReconnectEnabled) {
                Lifecycle = DeviceLifecycleState::Idle;
                RetryEligible = false;
                Publish();
                return;
            }
            ScheduleReconnect(DeviceConnectionResult::Failed, true, false);
            return;
        }
        RevokeStateChanged();
        Connection.reset();
        ++OperationEpoch;
        if (!OpenImmediately && IsIncomingEnabled && !wasEstablished) {
            Lifecycle = DeviceLifecycleState::Idle;
            Publish();
            StartConnection(DeviceOperationKind::IncomingEnable, false);
            return;
        }
        ScheduleReconnect(DeviceConnectionResult::Failed, RetryEligible, false);
    }

    void CompleteFailure(std::uint64_t epoch, DeviceConnectionResult result) {
        if (!IsCurrent(epoch)) return;
        RevokeStateChanged();
        if (!Connection) {
            ScheduleReconnect(result, RetryEligible, CurrentOperation == DeviceOperationKind::AutomaticReconnect);
            return;
        }
        Lifecycle = DeviceLifecycleState::Disconnecting;
        RestoreIncomingAfterClose = false;
        Publish(result);
        BeginClose(epoch,
                   CloseContinuation::Retry,
                   result,
                   RetryEligible,
                   CurrentOperation == DeviceOperationKind::AutomaticReconnect);
    }

    void BeginClose(std::uint64_t epoch,
                    CloseContinuation continuation,
                    DeviceConnectionResult failure = DeviceConnectionResult::Success,
                    bool shouldRetry = false,
                    bool completedAttempt = false) {
        if (IsCloseInFlight) {
            if (IsCloseContinuationAbandoned) return;
        }
        PendingCloseContinuation = continuation;
        PendingCloseFailure = failure;
        PendingCloseRetryEligible = shouldRetry;
        PendingCloseCompletedAttempt = completedAttempt;
        IsCloseContinuationAbandoned = false;
        if (IsCloseInFlight) return;
        if (!Connection) {
            ContinueAfterClose(epoch, false);
            return;
        }

        RevokeStateChanged();
        ClosingConnection = std::move(Connection);
        IsCloseInFlight = true;
        auto const closeBarrierEpoch = ++CloseBarrierEpoch;
        auto weak = weak_from_this();
        try {
            CloseBarrierTimer = TimerPlatform.Schedule(CloseBarrierTimeout, [weak, closeBarrierEpoch] {
                if (auto session = weak.lock()) {
                    try {
                        session->Post([weak, closeBarrierEpoch] {
                            if (auto current = weak.lock()) current->CompleteCloseBarrier(closeBarrierEpoch, true);
                        });
                    } catch (...) {
                    }
                }
            });
        } catch (...) {
            CloseBarrierTimer.reset();
        }
        if (!CloseBarrierTimer) {
            // The close remains unconfirmed, so retain ClosingConnection and IsCloseInFlight. The
            // operation itself is terminal, however, so callers must not await Disconnecting forever.
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(DeviceConnectionResult::Failed, true);
            AbandonCloseContinuation();
        }
        ClosingConnection->Close([weak, closeBarrierEpoch] {
            if (auto session = weak.lock()) {
                try {
                    session->Post([weak, closeBarrierEpoch] {
                        if (auto current = weak.lock()) current->CompleteCloseBarrier(closeBarrierEpoch, false);
                    });
                } catch (...) {
                }
            }
        });
    }

    void CompleteCloseBarrier(std::uint64_t closeBarrierEpoch, bool timedOut) {
        if (IsShutdown || !IsCloseInFlight || IsCloseCooldown || closeBarrierEpoch != CloseBarrierEpoch) return;
        CancelCloseBarrierTimer();
        if (timedOut) {
            // The old connection remains owned until its close callback arrives, but this operation is terminal.
            // Keeping the barrier busy prevents a replacement from overlapping the indeterminate close.
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(DeviceConnectionResult::TimedOut, true);
            AbandonCloseContinuation();
            return;
        }

        ClosingConnection.reset();
        StateChangedToken = 0;
        IsCloseCooldown = true;
        auto weak = weak_from_this();
        try {
            CloseBarrierTimer = TimerPlatform.Schedule(CloseBarrierCooldown, [weak, closeBarrierEpoch] {
                if (auto session = weak.lock()) {
                    try {
                        session->Post([weak, closeBarrierEpoch] {
                            if (auto current = weak.lock()) current->CompleteCloseCooldown(closeBarrierEpoch);
                        });
                    } catch (...) {
                    }
                }
            });
        } catch (...) {
            CloseBarrierTimer.reset();
        }
        if (!CloseBarrierTimer) CompleteCloseCooldown(closeBarrierEpoch);
    }

    void CompleteCloseCooldown(std::uint64_t closeBarrierEpoch) {
        if (IsShutdown || !IsCloseInFlight || !IsCloseCooldown || closeBarrierEpoch != CloseBarrierEpoch) return;
        CancelCloseBarrierTimer();
        IsCloseCooldown = false;
        IsCloseInFlight = false;
        ContinueAfterClose(OperationEpoch, false);
    }

    void AbandonCloseContinuation() noexcept {
        PendingCloseContinuation = CloseContinuation::Idle;
        PendingCloseFailure = DeviceConnectionResult::Success;
        PendingCloseRetryEligible = false;
        PendingCloseCompletedAttempt = false;
        RestoreIncomingAfterClose = false;
        IsCloseContinuationAbandoned = true;
    }

    void ContinueAfterClose(std::uint64_t epoch, bool) {
        if (!IsCurrent(epoch) || IsShutdown) return;
        auto const continuation = PendingCloseContinuation;
        auto const failure = PendingCloseFailure;
        auto const shouldRetry = PendingCloseRetryEligible;
        auto const completedAttempt = PendingCloseCompletedAttempt;
        PendingCloseContinuation = CloseContinuation::Idle;
        PendingCloseFailure = DeviceConnectionResult::Success;
        PendingCloseRetryEligible = false;
        PendingCloseCompletedAttempt = false;
        if (IsCloseContinuationAbandoned) {
            IsCloseContinuationAbandoned = false;
            if (IsSuspendClosePending) {
                IsSuspendClosePending = false;
                ResumeRequired = false;
                if (ResumeRequested) {
                    ResumeRequested = false;
                    IsSuspended = false;
                }
            }
            Lifecycle = DeviceLifecycleState::Idle;
            Publish();
            return;
        }
        if (continuation == CloseContinuation::Replace) {
            BeginCreate(epoch);
            return;
        }
        if (continuation == CloseContinuation::Retry) {
            ScheduleReconnect(failure, shouldRetry, completedAttempt);
            return;
        }
        if (continuation == CloseContinuation::Suspend) {
            CompleteSuspendClose(epoch);
            return;
        }
        Lifecycle = DeviceLifecycleState::Idle;
        Publish(failure);
        if (RestoreIncomingAfterClose && IsIncomingEnabled && !IsSuspended) {
            RestoreIncomingAfterClose = false;
            StartConnection(DeviceOperationKind::IncomingEnable, false);
        }
    }

    void ScheduleReconnect(DeviceConnectionResult result, bool shouldRetry, bool completedAttempt) {
        if (IsShutdown || IsSuspended || !shouldRetry || !IsReconnectEnabled || IsReconnectCancelled) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(result, true);
            return;
        }
        if (completedAttempt) ++CompletedRetryAttempts;
        auto const decision = ReconnectPolicy::Evaluate({
            .Request = ReconnectRequest::ConnectionLoss,
            .CompletedAttempts = CompletedRetryAttempts,
            .IsBlocked = false,
        });
        if (decision.Kind == ReconnectDecisionKind::TerminalFailure) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(result, true);
            return;
        }
        Lifecycle = DeviceLifecycleState::WaitingForReconnect;
        auto const timerEpoch = ++TimerEpoch;
        auto const operationEpoch = OperationEpoch;
        auto weak = weak_from_this();
        try {
            ReconnectTimer =
                TimerPlatform.Schedule(std::chrono::duration_cast<std::chrono::milliseconds>(decision.Delay),
                                       [weak, timerEpoch, operationEpoch] {
                                           if (auto session = weak.lock()) {
                                               session->Post([weak, timerEpoch, operationEpoch] {
                                                   if (auto current = weak.lock())
                                                       current->OnRetryTimer(timerEpoch, operationEpoch);
                                               });
                                           }
                                       });
        } catch (...) {
            ReconnectTimer.reset();
        }
        if (!ReconnectTimer) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(DeviceConnectionResult::Failed, true);
            return;
        }
        Publish(result);
    }

    void OnRetryTimer(std::uint64_t timerEpoch, std::uint64_t operationEpoch) {
        if (IsShutdown || IsSuspended || timerEpoch != TimerEpoch || operationEpoch != OperationEpoch ||
            Lifecycle != DeviceLifecycleState::WaitingForReconnect) {
            return;
        }
        ReconnectTimer.reset();
        StartConnection(DeviceOperationKind::AutomaticReconnect, OpenImmediately);
    }

    void CancelAutomaticReconnect() {
        CancelTimer();
        RetryEligible = false;
        bool const cancelsActiveReconnect = Lifecycle == DeviceLifecycleState::WaitingForReconnect ||
                                            (Lifecycle == DeviceLifecycleState::Connecting &&
                                             CurrentOperation == DeviceOperationKind::AutomaticReconnect);
        if (!cancelsActiveReconnect) {
            Publish(DeviceConnectionResult::Cancelled);
            return;
        }
        ++OperationEpoch;
        auto const epoch = OperationEpoch;
        if (Connection || IsCloseInFlight) {
            Lifecycle = DeviceLifecycleState::Disconnecting;
            RestoreIncomingAfterClose = false;
            RevokeStateChanged();
            BeginClose(epoch, CloseContinuation::Idle, DeviceConnectionResult::Cancelled);
            return;
        }
        Lifecycle = DeviceLifecycleState::Idle;
        Publish(DeviceConnectionResult::Cancelled);
    }

    [[nodiscard]] bool IsCurrent(std::uint64_t epoch) const noexcept { return !IsShutdown && epoch == OperationEpoch; }

    [[nodiscard]] bool IsCurrentConnection(std::uint64_t epoch,
                                           DeviceConnection const* expectedConnection) const noexcept {
        return IsCurrent(epoch) && Connection.get() == expectedConnection;
    }

    void CloseForShutdown() noexcept {
        CancelCloseBarrierTimer();
        RevokeStateChanged();
        if (Connection) Connection->Close({});
        Connection.reset();
        ClosingConnection.reset();
        IsCloseInFlight = false;
        IsCloseCooldown = false;
        IsCloseContinuationAbandoned = false;
        ++CloseBarrierEpoch;
        StateChangedToken = 0;
    }

    void BeginSuspendClose(std::uint64_t epoch) {
        IsSuspendClosePending = true;
        SuspendCloseEpoch = epoch;
        Lifecycle = DeviceLifecycleState::Disconnecting;
        RevokeStateChanged();
        BeginClose(epoch, CloseContinuation::Suspend, DeviceConnectionResult::Cancelled);
    }

    void CompleteSuspendClose(std::uint64_t epoch) {
        if (IsShutdown || !IsSuspended || !IsSuspendClosePending || SuspendCloseEpoch != epoch ||
            OperationEpoch != epoch) {
            return;
        }
        Connection.reset();
        StateChangedToken = 0;
        IsSuspendClosePending = false;
        Lifecycle = DeviceLifecycleState::Idle;
        Publish(DeviceConnectionResult::Cancelled);
        if (!ResumeRequested) return;
        ResumeRequested = false;
        IsSuspended = false;
        StartAfterResume();
    }

    void PostFailure(std::uint64_t epoch, DeviceConnection const* expectedConnection) noexcept {
        try {
            Post([weak = weak_from_this(), epoch, expectedConnection] {
                if (auto current = weak.lock()) {
                    if (current->IsCurrentConnection(epoch, expectedConnection))
                        current->CompleteFailure(epoch, DeviceConnectionResult::Failed);
                }
            });
        } catch (...) {
        }
    }

    void StartAfterResume() {
        if (ResumeRequired) {
            ResumeRequired = false;
            StartConnection(ResumeOpenImmediately ? DeviceOperationKind::Resume : DeviceOperationKind::IncomingEnable,
                            ResumeOpenImmediately);
        } else if (IsIncomingEnabled) {
            StartConnection(DeviceOperationKind::IncomingEnable, false);
        } else {
            Publish();
        }
    }
};

DeviceSession::DeviceSession(std::wstring deviceId,
                             std::wstring deviceName,
                             SerializedExecutor serializedExecutor,
                             DeviceConnectionPlatform& connectionPlatform,
                             DeviceTimerPlatform& timerPlatform,
                             FactSink factSink)
    : m_state(std::make_shared<State>(std::move(deviceId),
                                      std::move(deviceName),
                                      std::move(serializedExecutor),
                                      connectionPlatform,
                                      timerPlatform,
                                      std::move(factSink))) {
    if (!m_state->Executor) throw std::invalid_argument("DeviceSession requires a serialized executor");
}

DeviceSession::~DeviceSession() {
    Shutdown();
}

DeviceSessionSnapshot DeviceSession::Snapshot() const {
    return m_state ? m_state->Snapshot() : DeviceSessionSnapshot{};
}

bool DeviceSession::IsBusy() const noexcept {
    return m_state && m_state->IsBusy();
}

void DeviceSession::Rename(std::wstring deviceName) {
    if (!m_state || m_state->IsShutdown) return;
    m_state->DeviceName = std::move(deviceName);
    m_state->Publish();
}

void DeviceSession::HandleDeviceRemoved() {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->CancelTimer();
    state->RestoreIncomingAfterClose = false;
    bool const shouldRetry = state->RetryEligible;
    if (!state->Connection && !state->IsCloseInFlight && state->Lifecycle == DeviceLifecycleState::Idle &&
        !shouldRetry) {
        state->Publish();
        return;
    }
    bool const isIdleIncomingListener =
        state->Connection && state->Lifecycle == DeviceLifecycleState::Idle && state->IsIncomingEnabled && !shouldRetry;
    ++state->OperationEpoch;
    auto const epoch = state->OperationEpoch;
    if (isIdleIncomingListener) {
        state->Lifecycle = DeviceLifecycleState::Disconnecting;
        state->RevokeStateChanged();
        state->Publish();
        state->BeginClose(epoch, State::CloseContinuation::Idle);
        return;
    }
    if (state->IsCloseInFlight) {
        state->Lifecycle = DeviceLifecycleState::Disconnecting;
        state->Publish(DeviceConnectionResult::Failed);
        state->BeginClose(epoch, State::CloseContinuation::Retry, DeviceConnectionResult::Failed, shouldRetry);
        return;
    }
    if (!state->Connection) {
        state->ScheduleReconnect(DeviceConnectionResult::Failed, shouldRetry, false);
        return;
    }
    state->Lifecycle = DeviceLifecycleState::Disconnecting;
    state->RevokeStateChanged();
    state->Publish(DeviceConnectionResult::Failed);
    state->BeginClose(epoch, State::CloseContinuation::Retry, DeviceConnectionResult::Failed, shouldRetry);
}

void DeviceSession::Connect(DeviceOperationKind operation, bool openImmediately) {
    if (m_state) m_state->StartConnection(operation, openImmediately);
}

void DeviceSession::Disconnect(bool restoreIncoming) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->CancelTimer();
    state->IsReconnectCancelled = true;
    state->RetryEligible = false;
    ++state->OperationEpoch;
    auto const epoch = state->OperationEpoch;
    state->RestoreIncomingAfterClose = restoreIncoming;
    if (!state->Connection && !state->IsCloseInFlight) {
        state->Lifecycle = DeviceLifecycleState::Idle;
        state->Publish();
        return;
    }
    state->Lifecycle = DeviceLifecycleState::Disconnecting;
    state->RevokeStateChanged();
    state->Publish();
    state->BeginClose(epoch, State::CloseContinuation::Idle);
}

void DeviceSession::CancelReconnect() {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsReconnectCancelled = true;
    state->CancelAutomaticReconnect();
}

void DeviceSession::SetReconnectEnabled(bool enabled) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsReconnectEnabled = enabled;
    if (!enabled && (state->Lifecycle == DeviceLifecycleState::WaitingForReconnect ||
                     (state->Lifecycle == DeviceLifecycleState::Connecting &&
                      state->CurrentOperation == DeviceOperationKind::AutomaticReconnect))) {
        state->CancelAutomaticReconnect();
        return;
    }
    state->Publish();
}

void DeviceSession::SetIncomingEnabled(bool enabled) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsIncomingEnabled = enabled;
    bool const hasIncomingLifecycle =
        !state->OpenImmediately &&
        (state->Connection || state->IsCloseInFlight || state->Lifecycle == DeviceLifecycleState::WaitingForReconnect);
    if (!enabled && hasIncomingLifecycle) {
        Disconnect(false);
        return;
    }
    if (enabled && !state->Connection && !state->IsSuspended) {
        state->StartConnection(DeviceOperationKind::IncomingEnable, false);
        return;
    }
    state->Publish();
}

void DeviceSession::Suspend() {
    auto const state = m_state;
    if (!state || state->IsShutdown || state->IsSuspended) return;
    state->ResumeRequired = state->Connection || state->IsCloseInFlight || state->IsBusy();
    state->ResumeOpenImmediately = state->OpenImmediately;
    state->IsSuspended = true;
    state->CancelTimer();
    ++state->OperationEpoch;
    auto const epoch = state->OperationEpoch;
    if (state->Connection || state->IsCloseInFlight) {
        state->BeginSuspendClose(epoch);
        return;
    }
    state->Lifecycle = DeviceLifecycleState::Idle;
    state->Publish(DeviceConnectionResult::Cancelled);
}

void DeviceSession::Resume() {
    auto const state = m_state;
    if (!state || state->IsShutdown || !state->IsSuspended) return;
    if (state->IsSuspendClosePending) {
        state->ResumeRequested = true;
        return;
    }
    state->IsSuspended = false;
    state->StartAfterResume();
}

void DeviceSession::ResumeIdleAfterPowerTransition() {
    auto const state = m_state;
    // Active and incoming sessions remain suspended until the coordinator supplies the delayed resume targets.
    if (!state || state->IsShutdown || !state->IsSuspended || state->IsSuspendClosePending || state->ResumeRequired ||
        state->ResumeRequested || state->IsIncomingEnabled) {
        return;
    }
    state->IsSuspended = false;
    state->Publish();
}

void DeviceSession::Shutdown() noexcept {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsShutdown = true;
    ++state->OperationEpoch;
    state->CancelTimer();
    state->CloseForShutdown();
    state->Lifecycle = DeviceLifecycleState::Idle;
}

} // namespace apc::device
