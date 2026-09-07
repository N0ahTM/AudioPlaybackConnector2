#include <pch.h>

#include <core/AudioConnectionService.hpp>
#include <core/DeviceSession.hpp>
#include <core/ReconnectPolicy.hpp>

#include <array>
#include <stdexcept>
#include <utility>

#include <winerror.h>

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

[[nodiscard]] DeviceOpenResult
ToOpenResult(winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResult result) noexcept {
    using Status = winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
    auto const status = result.Status();
    return {
        .Result = ToConnectionResult(status),
        .IsTransientFailure = status == Status::RequestTimedOut ||
                              (status == Status::UnknownFailure &&
                               static_cast<HRESULT>(result.ExtendedError()) == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE)),
    };
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

    void Open(OpenCompletion completion) override { OpenDetached(m_connection, std::move(completion)); }

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
                                               OpenCompletion completion) {
        try {
            co_await winrt::resume_background();
            auto const result = co_await AudioConnectionService::OpenAsync(connection);
            if (completion) completion(ToOpenResult(result));
        } catch (winrt::hresult_error const&) {
            if (completion) completion({.Result = DeviceConnectionResult::Failed});
        } catch (...) {
            if (completion) completion({.Result = DeviceConnectionResult::Failed});
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
    enum class CloseContinuation { Idle, Replace, Retry, TransientOpenRetry, Suspend };

    static constexpr auto CloseBarrierTimeout = ReconnectPolicy::BlockedRetryDelay;
    static constexpr std::chrono::milliseconds CloseBarrierCooldown{1500};
    static constexpr std::size_t TransientOpenRetryMaxAttempts = 10;

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
    std::size_t TransientOpenRetryAttempts = 0;
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
    DeviceOperationKind ResumeOperation = DeviceOperationKind::Resume;
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
    DeviceDisconnectReason PendingCloseDisconnectReason = DeviceDisconnectReason::None;

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

    [[nodiscard]] bool IsLocallySuspended() const noexcept { return IsSuspended; }

    void Publish(DeviceConnectionResult result = DeviceConnectionResult::Success,
                 bool terminal = false,
                 DeviceDisconnectReason disconnectReason = DeviceDisconnectReason::None) {
        if (PublishFact)
            PublishFact({.Snapshot = Snapshot(),
                         .Operation = CurrentOperation,
                         .Result = result,
                         .IsTerminalFailure = terminal,
                         .DisconnectReason = disconnectReason});
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

        if (operation != DeviceOperationKind::AutomaticReconnect && operation != DeviceOperationKind::IncomingEnable) {
            IsReconnectCancelled = false;
            CompletedRetryAttempts = 0;
        }
        TransientOpenRetryAttempts = 0;
        CancelTimer();
        CurrentOperation = operation;
        RetryEligible = operation == DeviceOperationKind::AutomaticReconnect ||
                        operation == DeviceOperationKind::Startup || operation == DeviceOperationKind::Resume;
        OpenImmediately = openImmediately;
        ++OperationEpoch;
        auto const epoch = OperationEpoch;

        if (Connection) {
            Lifecycle = DeviceLifecycleState::Disconnecting;
            RestoreIncomingAfterClose = IsIncomingEnabled && !OpenImmediately;
            RevokeStateChanged();
            Publish(DeviceConnectionResult::Success, false, DeviceDisconnectReason::Normal);
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
            Connection->Open([weak, epoch, expectedConnection](DeviceOpenResult openResult) {
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

    void OnOpened(std::uint64_t epoch, DeviceConnection const* expectedConnection, DeviceOpenResult openResult) {
        if (!IsCurrentConnection(epoch, expectedConnection)) return;
        if (Lifecycle == DeviceLifecycleState::Connected) return;
        auto const result = openResult.Result;
        if (result != DeviceConnectionResult::Success) {
            CompleteFailure(epoch, result, openResult.IsTransientFailure);
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
                Publish(DeviceConnectionResult::Success, false, DeviceDisconnectReason::UnexpectedLoss);
                return;
            }
            ScheduleReconnect(DeviceConnectionResult::Failed, true, false, DeviceDisconnectReason::UnexpectedLoss);
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
        auto const disconnectReason =
            wasEstablished ? DeviceDisconnectReason::UnexpectedLoss : DeviceDisconnectReason::None;
        auto const completedAttempt = CurrentOperation == DeviceOperationKind::AutomaticReconnect;
        ScheduleReconnect(DeviceConnectionResult::Failed, RetryEligible, completedAttempt, disconnectReason);
    }

    void CompleteFailure(std::uint64_t epoch, DeviceConnectionResult result, bool isTransientOpenFailure = false) {
        if (!IsCurrent(epoch)) return;
        auto const shouldRetryTransientOpen = isTransientOpenFailure && OpenImmediately;
        RevokeStateChanged();
        if (!Connection) {
            if (shouldRetryTransientOpen) {
                ScheduleTransientOpenRetry(epoch, result);
                return;
            }
            ScheduleReconnect(result, RetryEligible, CurrentOperation == DeviceOperationKind::AutomaticReconnect);
            return;
        }
        Lifecycle = DeviceLifecycleState::Disconnecting;
        Publish(result);
        BeginClose(epoch,
                   shouldRetryTransientOpen ? CloseContinuation::TransientOpenRetry : CloseContinuation::Retry,
                   result,
                   RetryEligible,
                   CurrentOperation == DeviceOperationKind::AutomaticReconnect);
    }

    void BeginClose(std::uint64_t epoch,
                    CloseContinuation continuation,
                    DeviceConnectionResult failure = DeviceConnectionResult::Success,
                    bool shouldRetry = false,
                    bool completedAttempt = false,
                    DeviceDisconnectReason disconnectReason = DeviceDisconnectReason::None) {
        if (IsCloseInFlight) {
            if (IsCloseContinuationAbandoned) return;
        }
        PendingCloseContinuation = continuation;
        PendingCloseFailure = failure;
        PendingCloseRetryEligible = shouldRetry;
        PendingCloseCompletedAttempt = completedAttempt;
        PendingCloseDisconnectReason = disconnectReason;
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
            Publish(DeviceConnectionResult::Failed, true, PendingCloseDisconnectReason);
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
            Publish(DeviceConnectionResult::TimedOut, true, PendingCloseDisconnectReason);
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
                            if (auto current = weak.lock()) current->CompleteCloseCooldown(closeBarrierEpoch, true);
                        });
                    } catch (...) {
                    }
                }
            });
        } catch (...) {
            CloseBarrierTimer.reset();
        }
        if (!CloseBarrierTimer) CompleteCloseCooldown(closeBarrierEpoch, false);
    }

    void CompleteCloseCooldown(std::uint64_t closeBarrierEpoch, bool completedScheduledCooldown) {
        if (IsShutdown || !IsCloseInFlight || !IsCloseCooldown || closeBarrierEpoch != CloseBarrierEpoch) return;
        CancelCloseBarrierTimer();
        IsCloseCooldown = false;
        IsCloseInFlight = false;
        ContinueAfterClose(OperationEpoch, completedScheduledCooldown);
    }

    void AbandonCloseContinuation() noexcept {
        PendingCloseContinuation = CloseContinuation::Idle;
        PendingCloseFailure = DeviceConnectionResult::Success;
        PendingCloseRetryEligible = false;
        PendingCloseCompletedAttempt = false;
        PendingCloseDisconnectReason = DeviceDisconnectReason::None;
        IsCloseContinuationAbandoned = true;
    }

    void ContinueAfterClose(std::uint64_t epoch, bool completedScheduledCooldown) {
        if (!IsCurrent(epoch) || IsShutdown) return;
        auto const continuation = PendingCloseContinuation;
        auto const failure = PendingCloseFailure;
        auto const shouldRetry = PendingCloseRetryEligible;
        auto const completedAttempt = PendingCloseCompletedAttempt;
        auto const disconnectReason = PendingCloseDisconnectReason;
        PendingCloseContinuation = CloseContinuation::Idle;
        PendingCloseFailure = DeviceConnectionResult::Success;
        PendingCloseRetryEligible = false;
        PendingCloseCompletedAttempt = false;
        PendingCloseDisconnectReason = DeviceDisconnectReason::None;
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
            RestoreIncomingListenerAfterTerminal();
            return;
        }
        if (continuation == CloseContinuation::Replace) {
            BeginCreate(epoch);
            return;
        }
        if (continuation == CloseContinuation::Retry) {
            ScheduleReconnect(failure, shouldRetry, completedAttempt, disconnectReason);
            return;
        }
        if (continuation == CloseContinuation::TransientOpenRetry) {
            ScheduleTransientOpenRetry(epoch, failure, completedScheduledCooldown);
            return;
        }
        if (continuation == CloseContinuation::Suspend) {
            CompleteSuspendClose(epoch);
            return;
        }
        Lifecycle = DeviceLifecycleState::Idle;
        Publish(failure, false, disconnectReason);
        if (RestoreIncomingAfterClose && IsIncomingEnabled && !IsSuspended) {
            RestoreIncomingAfterClose = false;
            StartConnection(DeviceOperationKind::IncomingEnable, false);
        }
    }

    void ScheduleReconnect(DeviceConnectionResult result,
                           bool shouldRetry,
                           bool completedAttempt,
                           DeviceDisconnectReason disconnectReason = DeviceDisconnectReason::None) {
        if (IsShutdown || IsSuspended || !shouldRetry || !IsReconnectEnabled || IsReconnectCancelled) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(result, true, disconnectReason);
            RestoreIncomingListenerAfterTerminal();
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
            Publish(result, true, disconnectReason);
            RestoreIncomingListenerAfterTerminal();
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
            Publish(DeviceConnectionResult::Failed, true, disconnectReason);
            RestoreIncomingListenerAfterTerminal();
            return;
        }
        Publish(result, false, disconnectReason);
    }

    void OnRetryTimer(std::uint64_t timerEpoch, std::uint64_t operationEpoch) {
        if (IsShutdown || IsSuspended || timerEpoch != TimerEpoch || operationEpoch != OperationEpoch ||
            Lifecycle != DeviceLifecycleState::WaitingForReconnect) {
            return;
        }
        ReconnectTimer.reset();
        StartConnection(DeviceOperationKind::AutomaticReconnect, OpenImmediately);
    }

    [[nodiscard]] static std::chrono::milliseconds TransientOpenRetryDelay(std::size_t failedAttempt) noexcept {
        constexpr std::array<int, TransientOpenRetryMaxAttempts - 1> delaysMs{
            500, 1000, 1500, 2500, 4000, 6000, 8000, 8000, 8000};
        return std::chrono::milliseconds(delaysMs[failedAttempt - 1]);
    }

    void ScheduleTransientOpenRetry(std::uint64_t epoch,
                                    DeviceConnectionResult result,
                                    bool completedScheduledCooldown = false) {
        if (!IsCurrent(epoch) || IsShutdown || IsSuspended || IsReconnectCancelled) return;
        if (++TransientOpenRetryAttempts >= TransientOpenRetryMaxAttempts) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(result, true);
            RestoreIncomingListenerAfterTerminal();
            return;
        }
        Lifecycle = DeviceLifecycleState::WaitingForReconnect;
        auto delay = TransientOpenRetryDelay(TransientOpenRetryAttempts);
        if (completedScheduledCooldown && delay > CloseBarrierCooldown) {
            delay -= CloseBarrierCooldown;
        } else if (completedScheduledCooldown) {
            delay = std::chrono::milliseconds::zero();
        }
        auto const timerEpoch = ++TimerEpoch;
        auto weak = weak_from_this();
        try {
            ReconnectTimer = TimerPlatform.Schedule(delay, [weak, timerEpoch, epoch] {
                if (auto session = weak.lock()) {
                    session->Post([weak, timerEpoch, epoch] {
                        if (auto current = weak.lock()) current->OnTransientOpenRetryTimer(timerEpoch, epoch);
                    });
                }
            });
        } catch (...) {
            ReconnectTimer.reset();
        }
        if (!ReconnectTimer) {
            Lifecycle = DeviceLifecycleState::Failed;
            Publish(DeviceConnectionResult::Failed, true);
            RestoreIncomingListenerAfterTerminal();
            return;
        }
        Publish(result);
    }

    void OnTransientOpenRetryTimer(std::uint64_t timerEpoch, std::uint64_t operationEpoch) {
        if (IsShutdown || IsSuspended || timerEpoch != TimerEpoch || operationEpoch != OperationEpoch ||
            Lifecycle != DeviceLifecycleState::WaitingForReconnect) {
            return;
        }
        ReconnectTimer.reset();
        BeginCreate(operationEpoch);
    }

    void RestoreIncomingListenerAfterTerminal() {
        if (!RestoreIncomingAfterClose && (!IsIncomingEnabled || !OpenImmediately)) return;
        RestoreIncomingAfterClose = false;
        if (!IsShutdown && !IsSuspended && IsIncomingEnabled && OpenImmediately) {
            StartConnection(DeviceOperationKind::IncomingEnable, false);
        }
    }

    void CancelAutomaticReconnect(bool restoreIncoming) {
        CancelTimer();
        RetryEligible = false;
        bool const cancelsActiveReconnect = Lifecycle == DeviceLifecycleState::WaitingForReconnect ||
                                            (Lifecycle == DeviceLifecycleState::Connecting &&
                                             CurrentOperation == DeviceOperationKind::AutomaticReconnect) ||
                                            (Lifecycle == DeviceLifecycleState::Disconnecting &&
                                             CurrentOperation == DeviceOperationKind::AutomaticReconnect);
        if (!cancelsActiveReconnect) {
            Publish(DeviceConnectionResult::Cancelled);
            return;
        }
        ++OperationEpoch;
        auto const epoch = OperationEpoch;
        if (Connection || IsCloseInFlight) {
            Lifecycle = DeviceLifecycleState::Disconnecting;
            RestoreIncomingAfterClose = restoreIncoming && IsIncomingEnabled;
            RevokeStateChanged();
            BeginClose(epoch, CloseContinuation::Idle, DeviceConnectionResult::Cancelled);
            return;
        }
        Lifecycle = DeviceLifecycleState::Idle;
        Publish(DeviceConnectionResult::Cancelled);
        if (restoreIncoming && IsIncomingEnabled) StartConnection(DeviceOperationKind::IncomingEnable, false);
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
            StartConnection(ResumeOperation, ResumeOpenImmediately);
        } else if (IsIncomingEnabled) {
            StartConnection(DeviceOperationKind::IncomingEnable, false);
        } else {
            Publish();
        }
    }

    void SupersedePowerTransitionRecovery(DeviceOperationKind operation, bool openImmediately) {
        IsSuspended = false;
        ResumeRequired = false;
        ResumeRequested = false;
        IsSuspendClosePending = false;

        if (!IsCloseInFlight) {
            StartConnection(operation, openImmediately);
            return;
        }
        if (IsCloseContinuationAbandoned) return;

        IsReconnectCancelled = false;
        CompletedRetryAttempts = 0;
        TransientOpenRetryAttempts = 0;
        CancelTimer();
        CurrentOperation = operation;
        RetryEligible = false;
        OpenImmediately = openImmediately;
        ++OperationEpoch;
        auto const epoch = OperationEpoch;
        Lifecycle = DeviceLifecycleState::Disconnecting;
        RestoreIncomingAfterClose = IsIncomingEnabled && !OpenImmediately;
        Publish();
        BeginClose(epoch, CloseContinuation::Replace);
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

bool DeviceSession::IsSuspended() const noexcept {
    return m_state && m_state->IsLocallySuspended();
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
        state->Publish(DeviceConnectionResult::Failed, false, DeviceDisconnectReason::DeviceRemoved);
        state->BeginClose(epoch,
                          State::CloseContinuation::Retry,
                          DeviceConnectionResult::Failed,
                          shouldRetry,
                          false,
                          DeviceDisconnectReason::DeviceRemoved);
        return;
    }
    if (!state->Connection) {
        state->ScheduleReconnect(
            DeviceConnectionResult::Failed, shouldRetry, false, DeviceDisconnectReason::DeviceRemoved);
        return;
    }
    state->Lifecycle = DeviceLifecycleState::Disconnecting;
    state->RevokeStateChanged();
    state->Publish(DeviceConnectionResult::Failed, false, DeviceDisconnectReason::DeviceRemoved);
    state->BeginClose(epoch,
                      State::CloseContinuation::Retry,
                      DeviceConnectionResult::Failed,
                      shouldRetry,
                      false,
                      DeviceDisconnectReason::DeviceRemoved);
}

void DeviceSession::Connect(DeviceOperationKind operation, bool openImmediately) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    if (state->IsSuspended &&
        (operation == DeviceOperationKind::ManualConnect || operation == DeviceOperationKind::ManualReconnect)) {
        state->SupersedePowerTransitionRecovery(operation, openImmediately);
        return;
    }
    state->StartConnection(operation, openImmediately);
}

bool DeviceSession::Disconnect(bool restoreIncoming) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return false;
    if (state->IsCloseInFlight && state->IsCloseContinuationAbandoned) {
        // The unconfirmed close owns the connection until its callback arrives. A second close would
        // only make the terminal timeout appear active again and cannot safely change its continuation.
        return false;
    }
    state->CancelTimer();
    state->IsReconnectCancelled = true;
    state->RetryEligible = false;
    ++state->OperationEpoch;
    auto const epoch = state->OperationEpoch;
    state->RestoreIncomingAfterClose = restoreIncoming;
    if (!state->Connection && !state->IsCloseInFlight) {
        state->Lifecycle = DeviceLifecycleState::Idle;
        state->Publish(DeviceConnectionResult::Success, false, DeviceDisconnectReason::Normal);
        if (state->RestoreIncomingAfterClose && state->IsIncomingEnabled && !state->IsSuspended) {
            state->RestoreIncomingAfterClose = false;
            state->StartConnection(DeviceOperationKind::IncomingEnable, false);
        }
        return true;
    }
    state->Lifecycle = DeviceLifecycleState::Disconnecting;
    state->RevokeStateChanged();
    state->Publish(DeviceConnectionResult::Success, false, DeviceDisconnectReason::Normal);
    state->BeginClose(epoch, State::CloseContinuation::Idle);
    return true;
}

void DeviceSession::CancelReconnect() {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsReconnectCancelled = true;
    state->CancelAutomaticReconnect(state->IsIncomingEnabled);
}

void DeviceSession::SetReconnectEnabled(bool enabled) {
    auto const state = m_state;
    if (!state || state->IsShutdown) return;
    state->IsReconnectEnabled = enabled;
    if (!enabled && (state->Lifecycle == DeviceLifecycleState::WaitingForReconnect ||
                     ((state->Lifecycle == DeviceLifecycleState::Connecting ||
                       state->Lifecycle == DeviceLifecycleState::Disconnecting) &&
                      state->CurrentOperation == DeviceOperationKind::AutomaticReconnect))) {
        state->CancelAutomaticReconnect(state->IsIncomingEnabled);
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
    state->ResumeOperation =
        state->ResumeOpenImmediately ? DeviceOperationKind::Resume : DeviceOperationKind::IncomingEnable;
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

void DeviceSession::CancelPowerTransitionRecovery() {
    auto const state = m_state;
    if (!state || state->IsShutdown || !state->IsSuspended) return;

    // The delayed recovery was superseded on the serialized context. Release only its local hold;
    // the superseding operation still owns the current lifecycle and any close barrier.
    state->IsSuspended = false;
    state->ResumeRequired = false;
    state->ResumeRequested = false;
    state->IsSuspendClosePending = false;
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
