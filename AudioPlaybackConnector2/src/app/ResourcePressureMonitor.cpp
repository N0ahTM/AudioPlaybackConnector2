#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <app/ResourcePressureMonitor.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace {
thread_local void const* g_activeResourcePressureCallback = nullptr;

std::optional<bool> QueryMemoryState(HANDLE notification) noexcept {
    BOOL signaled = FALSE;
    if (!QueryMemoryResourceNotification(notification, &signaled)) return std::nullopt;
    return signaled != FALSE;
}

std::optional<UserActivityState> QueryUserActivity() noexcept {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    if (FAILED(SHQueryUserNotificationState(&state))) return std::nullopt;

    switch (state) {
        case QUNS_BUSY: return UserActivityState::Busy;
        case QUNS_RUNNING_D3D_FULL_SCREEN: return UserActivityState::Fullscreen;
        case QUNS_PRESENTATION_MODE: return UserActivityState::Presentation;
        case QUNS_QUIET_TIME: return UserActivityState::QuietTime;
        case QUNS_APP: return UserActivityState::ImmersiveApp;
        case QUNS_NOT_PRESENT: return UserActivityState::NotPresent;
        case QUNS_ACCEPTS_NOTIFICATIONS: return UserActivityState::Available;
        default: return UserActivityState::Unknown;
    }
}

std::optional<bool> QueryEnergySaver() noexcept {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) return std::nullopt;
    return status.SystemStatusFlag != 0;
}

DWORD PollPeriod(std::chrono::milliseconds interval) noexcept {
    constexpr std::chrono::milliseconds c_defaultPollInterval{std::chrono::seconds{5}};
    constexpr std::chrono::milliseconds c_minimumPollInterval{std::chrono::seconds{1}};
    constexpr auto maximum = static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<DWORD>::max());
    if (interval <= std::chrono::milliseconds::zero()) {
        interval = c_defaultPollInterval;
    }
    return static_cast<DWORD>(std::clamp(interval.count(), c_minimumPollInterval.count(), maximum));
}

FILETIME RelativeDueTime(std::chrono::milliseconds delay) noexcept {
    constexpr std::int64_t c_fileTimeUnitsPerMillisecond = 10'000;
    const auto bounded = std::clamp<std::int64_t>(
        delay.count(), 1, std::numeric_limits<std::int64_t>::max() / c_fileTimeUnitsPerMillisecond);
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<ULONGLONG>(-bounded * c_fileTimeUnitsPerMillisecond);
    return {.dwLowDateTime = value.LowPart, .dwHighDateTime = value.HighPart};
}
} // namespace

struct ResourcePressureMonitor::Impl {
    struct RunContext : std::enable_shared_from_this<RunContext> {
        explicit RunContext(Callback callback,
                            Config config,
                            std::shared_ptr<std::atomic_uint64_t> sequence,
                            void const* ownerIdentity,
                            std::shared_ptr<std::atomic_uint64_t> ownerEpoch,
                            std::uint64_t epoch)
            : Handler(std::move(callback)), NormalPeriod(PollPeriod(config.PollInterval)),
              ConstrainedPeriod(PollPeriod(config.ConstrainedPollInterval)), Sequence(std::move(sequence)),
              OwnerIdentity(ownerIdentity), OwnerEpoch(std::move(ownerEpoch)), Epoch(epoch),
              HeartbeatInterval(std::max(config.SnapshotHeartbeatInterval, std::chrono::milliseconds{1})) {}

        ~RunContext() { Shutdown(true); }

        [[nodiscard]] bool Initialize() noexcept {
            LowMemory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
            if (!LowMemory) return false;
            HighMemory = CreateMemoryResourceNotification(HighMemoryResourceNotification);
            if (!HighMemory) return false;

            LowWait = CreateThreadpoolWait(&MemoryCallback, this, nullptr);
            if (!LowWait) return false;
            HighWait = CreateThreadpoolWait(&MemoryCallback, this, nullptr);
            if (!HighWait) return false;
            PollTimer = CreateThreadpoolTimer(&TimerCallback, this, nullptr);
            return PollTimer != nullptr;
        }

        void Arm() noexcept {
            std::scoped_lock lock(ControlMutex);
            Running.store(true);
            SetThreadpoolWait(LowWait, LowMemory, nullptr);
            SetThreadpoolWait(HighWait, HighMemory, nullptr);
            auto dueTime = RelativeDueTime(std::chrono::milliseconds{1});
            SetThreadpoolTimer(PollTimer, &dueTime, 0, std::min<DWORD>(NormalPeriod / 5, 1000));
        }

        [[nodiscard]] bool RequestProbe() noexcept {
            try {
                std::scoped_lock lock(ControlMutex);
                if (!Running.load() || !IsOwnerEpochCurrent() || !PollTimer) return false;
                auto dueTime = RelativeDueTime(std::chrono::milliseconds{1});
                SetThreadpoolTimer(PollTimer, &dueTime, 0, 0);
                return true;
            } catch (...) {
                return false;
            }
        }

        void Shutdown(bool waitForPublicCallbacks) noexcept {
            if (ShutdownStarted.exchange(true)) return;
            Running.store(false);

            {
                std::scoped_lock lock(ControlMutex);
                if (LowWait) SetThreadpoolWait(LowWait, nullptr, nullptr);
                if (HighWait) SetThreadpoolWait(HighWait, nullptr, nullptr);
                if (PollTimer) SetThreadpoolTimer(PollTimer, nullptr, 0, 0);
            }

            if (LowWait) WaitForThreadpoolWaitCallbacks(LowWait, TRUE);
            if (HighWait) WaitForThreadpoolWaitCallbacks(HighWait, TRUE);
            if (PollTimer) WaitForThreadpoolTimerCallbacks(PollTimer, TRUE);

            if (LowWait) {
                CloseThreadpoolWait(std::exchange(LowWait, nullptr));
            }
            if (HighWait) {
                CloseThreadpoolWait(std::exchange(HighWait, nullptr));
            }
            if (PollTimer) {
                CloseThreadpoolTimer(std::exchange(PollTimer, nullptr));
            }
            if (LowMemory) {
                CloseHandle(std::exchange(LowMemory, nullptr));
            }
            if (HighMemory) {
                CloseHandle(std::exchange(HighMemory, nullptr));
            }

            if (waitForPublicCallbacks && g_activeResourcePressureCallback != OwnerIdentity) {
                WaitForPublicCallbacks();
            }
        }

        [[nodiscard]] bool IsRunning() const noexcept { return Running.load(); }

        [[nodiscard]] bool HasPublicCallbacks() noexcept {
            std::scoped_lock lock(PublicCallbackMutex);
            return PublicCallbacks != 0;
        }

        void WaitForPublicCallbacks() noexcept {
            std::unique_lock lock(PublicCallbackMutex);
            PublicCallbackFinished.wait(lock, [this] { return PublicCallbacks == 0; });
        }

    private:
        struct PublicCallbackGuard {
            explicit PublicCallbackGuard(std::shared_ptr<RunContext> owner) : Owner(std::move(owner)) {}
            ~PublicCallbackGuard() { Owner->FinishPublicCallback(); }
            std::shared_ptr<RunContext> Owner;
        };

        struct ActiveCallbackScope {
            explicit ActiveCallbackScope(void const* current) noexcept
                : Previous(std::exchange(g_activeResourcePressureCallback, current)) {}
            ~ActiveCallbackScope() { g_activeResourcePressureCallback = Previous; }
            void const* Previous;
        };

        static void CALLBACK MemoryCallback(PTP_CALLBACK_INSTANCE instance,
                                            void* context,
                                            PTP_WAIT,
                                            TP_WAIT_RESULT) noexcept {
            try {
                static_cast<RunContext*>(context)->shared_from_this()->ProbeAndPublish(instance);
            } catch (...) {
            }
        }

        static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
            try {
                static_cast<RunContext*>(context)->shared_from_this()->ProbeAndPublish(instance);
            } catch (...) {
            }
        }

        void ProbeAndPublish(PTP_CALLBACK_INSTANCE instance) {
            bool deliverSnapshots = false;
            {
                std::scoped_lock lock(ControlMutex);
                if (!Running.load() || !IsOwnerEpochCurrent()) return;

                ResourcePressureProbe probe{
                    .LowMemorySignaled = QueryMemoryState(LowMemory),
                    .HighMemorySignaled = QueryMemoryState(HighMemory),
                    .MemoryProbeAttempted = true,
                };
                probe.UserActivity = QueryUserActivity();
                probe.EnergySaver = QueryEnergySaver();
                probe.UserActivityProbeAttempted = true;
                probe.EnergySaverProbeAttempted = true;
                const auto values = Reducer.Apply(probe);
                ArmMemoryWaits(probe.LowMemorySignaled, probe.HighMemorySignaled);
                const bool probesComplete = probe.LowMemorySignaled.has_value() &&
                                            probe.HighMemorySignaled.has_value() && probe.UserActivity.has_value() &&
                                            probe.EnergySaver.has_value();
                ScheduleNextPoll(ShouldUseConstrainedPollInterval(values, probesComplete));

                auto const observedAt = std::chrono::steady_clock::now();
                const bool heartbeatDue =
                    (!LastSnapshotPublishedAt || observedAt - *LastSnapshotPublishedAt >= HeartbeatInterval);
                if (!LastPublished || *LastPublished != values || heartbeatDue) {
                    if (Handler) {
                        PendingSnapshots.push_back({
                            .Values = values,
                            .ObservedAt = observedAt,
                            .Sequence = Sequence->fetch_add(1) + 1,
                        });
                        LastPublished = values;
                        LastSnapshotPublishedAt = observedAt;
                        if (DeliveryActive) return;
                        DeliveryActive = true;
                        std::scoped_lock callbackLock(PublicCallbackMutex);
                        ++PublicCallbacks;
                        deliverSnapshots = true;
                    } else {
                        LastPublished = values;
                        LastSnapshotPublishedAt = observedAt;
                    }
                }
            }

            if (!deliverSnapshots) return;
            PublicCallbackGuard callbackGuard(shared_from_this());
            DisassociateCurrentThreadFromCallback(instance);
            ActiveCallbackScope callbackScope(OwnerIdentity);
            for (;;) {
                std::optional<ResourcePressureSnapshot> snapshot;
                {
                    std::scoped_lock lock(ControlMutex);
                    if (!Running.load() || !IsOwnerEpochCurrent() || PendingSnapshots.empty()) {
                        PendingSnapshots.clear();
                        DeliveryActive = false;
                        break;
                    }
                    snapshot = PendingSnapshots.front();
                    PendingSnapshots.pop_front();
                }
                try {
                    Handler(*snapshot);
                } catch (...) {
                }
            }
        }

        void ArmMemoryWaits(std::optional<bool> lowMemorySignaled, std::optional<bool> highMemorySignaled) noexcept {
            auto const plan = PlanMemoryNotificationWaits(lowMemorySignaled, highMemorySignaled);
            SetThreadpoolWait(LowWait, plan.ArmLow ? LowMemory : nullptr, nullptr);
            SetThreadpoolWait(HighWait, plan.ArmHigh ? HighMemory : nullptr, nullptr);
        }

        void ScheduleNextPoll(bool constrained) noexcept {
            auto const period = constrained ? ConstrainedPeriod : NormalPeriod;
            auto dueTime = RelativeDueTime(std::chrono::milliseconds{period});
            SetThreadpoolTimer(PollTimer, &dueTime, 0, std::min<DWORD>(period / 5, 1000));
        }

        [[nodiscard]] bool IsOwnerEpochCurrent() const noexcept { return OwnerEpoch->load() == Epoch; }

        void FinishPublicCallback() noexcept {
            std::scoped_lock lock(PublicCallbackMutex);
            --PublicCallbacks;
            if (PublicCallbacks == 0) {
                PublicCallbackFinished.notify_all();
            }
        }

        Callback Handler;
        DWORD NormalPeriod = 0;
        DWORD ConstrainedPeriod = 0;
        std::atomic_bool Running = false;
        std::atomic_bool ShutdownStarted = false;
        std::mutex ControlMutex;
        std::mutex PublicCallbackMutex;
        std::condition_variable PublicCallbackFinished;
        std::size_t PublicCallbacks = 0;
        ResourcePressureStateReducer Reducer;
        std::optional<ResourcePressureValues> LastPublished;
        std::optional<std::chrono::steady_clock::time_point> LastSnapshotPublishedAt;
        std::deque<ResourcePressureSnapshot> PendingSnapshots;
        std::shared_ptr<std::atomic_uint64_t> Sequence;
        void const* OwnerIdentity = nullptr;
        std::shared_ptr<std::atomic_uint64_t> OwnerEpoch;
        std::uint64_t Epoch = 0;
        std::chrono::milliseconds HeartbeatInterval;
        bool DeliveryActive = false;
        HANDLE LowMemory = nullptr;
        HANDLE HighMemory = nullptr;
        PTP_WAIT LowWait = nullptr;
        PTP_WAIT HighWait = nullptr;
        PTP_TIMER PollTimer = nullptr;
    };

    explicit Impl(Callback callback, Config config) : Handler(std::move(callback)), MonitorConfig(config) {
        DeferredStopWork = CreateThreadpoolWork(&DeferredStopCallback, this, nullptr);
    }

    ~Impl() {
        Stop();
        if (DeferredStopWork) {
            WaitForThreadpoolWorkCallbacks(DeferredStopWork, TRUE);
            CloseThreadpoolWork(std::exchange(DeferredStopWork, nullptr));
        }
    }

    [[nodiscard]] bool Start() noexcept {
        try {
            if (g_activeResourcePressureCallback == this) return false;
            if (!DeferredStopWork) return false;
            std::scoped_lock lock(LifecycleMutex);
            auto const pendingStop = StopRequestFlags.exchange(0);
            if ((pendingStop & c_stopRequested) != 0) {
                auto const originatedFromCallback = (pendingStop & c_stopOriginatedFromCallback) != 0;
                StopWhileLocked(originatedFromCallback);
                if (originatedFromCallback) return false;
            }
            if (Running.load()) return true;
            DrainRetiringContextWhileLocked();

            auto const epoch = Epoch->fetch_add(1) + 1;
            auto context = std::make_shared<RunContext>(Handler, MonitorConfig, Sequence, this, Epoch, epoch);
            if (!context->Initialize()) return false;
            Active = context;
            Running.store(true);
            context->Arm();
            auto const stopAfterArm = StopRequestFlags.exchange(0);
            if ((stopAfterArm & c_stopRequested) != 0) {
                auto const originatedFromCallback = (stopAfterArm & c_stopOriginatedFromCallback) != 0;
                StopWhileLocked(originatedFromCallback);
                return !originatedFromCallback;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void Stop() noexcept {
        auto const calledFromCallback = g_activeResourcePressureCallback == this;
        Running.store(false);
        Epoch->fetch_add(1);
        StopRequestFlags.fetch_or(c_stopRequested | (calledFromCallback ? c_stopOriginatedFromCallback : 0));
        if (calledFromCallback) {
            std::unique_lock lock(LifecycleMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                if (DeferredStopWork) SubmitThreadpoolWork(DeferredStopWork);
                return;
            }
            auto const pendingStop = StopRequestFlags.exchange(0);
            StopWhileLocked((pendingStop & c_stopOriginatedFromCallback) != 0);
            return;
        }
        std::scoped_lock lock(LifecycleMutex);
        static_cast<void>(StopRequestFlags.exchange(0));
        StopWhileLocked(false);
    }

    [[nodiscard]] bool IsRunning() const noexcept { return Running.load(); }

    [[nodiscard]] bool RequestProbe() noexcept {
        try {
            std::shared_ptr<RunContext> context;
            {
                std::scoped_lock lock(LifecycleMutex);
                if (!Running.load() || !Active) return false;
                context = Active;
            }
            return context->RequestProbe();
        } catch (...) {
            return false;
        }
    }

    static void CALLBACK DeferredStopCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
        auto self = static_cast<Impl*>(context);
        std::scoped_lock lock(self->LifecycleMutex);
        auto const pendingStop = self->StopRequestFlags.exchange(0);
        if ((pendingStop & c_stopRequested) == 0) return;
        self->StopWhileLocked((pendingStop & c_stopOriginatedFromCallback) != 0);
    }

    void StopWhileLocked(bool calledFromCallback) noexcept {
        Running.store(false);
        auto context = std::exchange(Active, nullptr);
        if (context) {
            context->Shutdown(!calledFromCallback);
            if (calledFromCallback && context->HasPublicCallbacks()) {
                Retiring = std::move(context);
            }
        }
        if (!calledFromCallback) {
            DrainRetiringContextWhileLocked();
        }
    }

    void DrainRetiringContextWhileLocked() noexcept {
        if (!Retiring) return;
        Retiring->WaitForPublicCallbacks();
        Retiring.reset();
    }

    Callback Handler;
    static constexpr std::uint32_t c_stopRequested = 0x1;
    static constexpr std::uint32_t c_stopOriginatedFromCallback = 0x2;
    Config MonitorConfig;
    std::shared_ptr<std::atomic_uint64_t> Sequence = std::make_shared<std::atomic_uint64_t>(std::uint64_t{0});
    std::shared_ptr<std::atomic_uint64_t> Epoch = std::make_shared<std::atomic_uint64_t>(std::uint64_t{0});
    std::atomic_bool Running = false;
    std::atomic_uint32_t StopRequestFlags = 0;
    mutable std::mutex LifecycleMutex;
    std::shared_ptr<RunContext> Active;
    std::shared_ptr<RunContext> Retiring;
    PTP_WORK DeferredStopWork = nullptr;
};

ResourcePressureMonitor::ResourcePressureMonitor(Callback callback, Config config)
    : m_impl(std::make_unique<Impl>(std::move(callback), config)) {}

ResourcePressureMonitor::~ResourcePressureMonitor() {
    Stop();
}

bool ResourcePressureMonitor::Start() noexcept {
    return m_impl->Start();
}

bool ResourcePressureMonitor::RequestProbe() noexcept {
    return m_impl->RequestProbe();
}

void ResourcePressureMonitor::Stop() noexcept {
    m_impl->Stop();
}

bool ResourcePressureMonitor::IsRunning() const noexcept {
    return m_impl->IsRunning();
}
