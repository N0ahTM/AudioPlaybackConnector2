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
                            void const* ownerIdentity)
            : Handler(std::move(callback)), Period(PollPeriod(config.PollInterval)), Sequence(std::move(sequence)),
              OwnerIdentity(ownerIdentity) {}

        ~RunContext() { Shutdown(); }

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
            SetThreadpoolTimer(PollTimer, &dueTime, Period, std::min<DWORD>(Period / 5, 1000));
        }

        void Shutdown() noexcept {
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

            if (g_activeResourcePressureCallback != OwnerIdentity) {
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
                static_cast<RunContext*>(context)->shared_from_this()->ProbeAndPublish(instance, false);
            } catch (...) {
            }
        }

        static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
            try {
                static_cast<RunContext*>(context)->shared_from_this()->ProbeAndPublish(instance, true);
            } catch (...) {
            }
        }

        void ProbeAndPublish(PTP_CALLBACK_INSTANCE instance, bool includePeriodicSignals) {
            bool deliverSnapshots = false;
            {
                std::scoped_lock lock(ControlMutex);
                if (!Running.load()) return;

                ResourcePressureProbe probe{
                    .LowMemorySignaled = QueryMemoryState(LowMemory),
                    .HighMemorySignaled = QueryMemoryState(HighMemory),
                };
                if (includePeriodicSignals) {
                    probe.UserActivity = QueryUserActivity();
                    probe.EnergySaver = QueryEnergySaver();
                }
                const auto values = Reducer.Apply(probe);
                ArmMemoryWaits(values.Memory);

                if (!LastPublished || *LastPublished != values) {
                    LastPublished = values;
                    if (Handler) {
                        PendingSnapshots.push_back({
                            .Values = values,
                            .ObservedAt = std::chrono::steady_clock::now(),
                            .Sequence = Sequence->fetch_add(1) + 1,
                        });
                        if (DeliveryActive) return;
                        DeliveryActive = true;
                        std::scoped_lock callbackLock(PublicCallbackMutex);
                        ++PublicCallbacks;
                        deliverSnapshots = true;
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
                    if (!Running.load() || PendingSnapshots.empty()) {
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

        void ArmMemoryWaits(MemoryPressureState state) noexcept {
            SetThreadpoolWait(LowWait, state == MemoryPressureState::Low ? nullptr : LowMemory, nullptr);
            SetThreadpoolWait(HighWait, state == MemoryPressureState::High ? nullptr : HighMemory, nullptr);
        }

        void FinishPublicCallback() noexcept {
            std::scoped_lock lock(PublicCallbackMutex);
            --PublicCallbacks;
            if (PublicCallbacks == 0) {
                PublicCallbackFinished.notify_all();
            }
        }

        Callback Handler;
        DWORD Period = 0;
        std::atomic_bool Running = false;
        std::atomic_bool ShutdownStarted = false;
        std::mutex ControlMutex;
        std::mutex PublicCallbackMutex;
        std::condition_variable PublicCallbackFinished;
        std::size_t PublicCallbacks = 0;
        ResourcePressureStateReducer Reducer;
        std::optional<ResourcePressureValues> LastPublished;
        std::deque<ResourcePressureSnapshot> PendingSnapshots;
        std::shared_ptr<std::atomic_uint64_t> Sequence;
        void const* OwnerIdentity = nullptr;
        bool DeliveryActive = false;
        HANDLE LowMemory = nullptr;
        HANDLE HighMemory = nullptr;
        PTP_WAIT LowWait = nullptr;
        PTP_WAIT HighWait = nullptr;
        PTP_TIMER PollTimer = nullptr;
    };

    explicit Impl(Callback callback, Config config) : Handler(std::move(callback)), MonitorConfig(config) {}

    [[nodiscard]] bool Start() noexcept {
        try {
            if (g_activeResourcePressureCallback == this) return false;
            std::scoped_lock lock(LifecycleMutex);
            if (Running.load()) return true;
            DrainRetiringContextWhileLocked();

            auto context = std::make_shared<RunContext>(Handler, MonitorConfig, Sequence, this);
            if (!context->Initialize()) return false;
            Active = context;
            Running.store(true);
            context->Arm();
            return true;
        } catch (...) {
            return false;
        }
    }

    void Stop() noexcept {
        if (g_activeResourcePressureCallback == this) {
            std::unique_lock lock(LifecycleMutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            StopWhileLocked(true);
            return;
        }
        std::scoped_lock lock(LifecycleMutex);
        StopWhileLocked(false);
    }

    [[nodiscard]] bool IsRunning() const noexcept { return Running.load(); }

    void StopWhileLocked(bool calledFromCallback) noexcept {
        Running.store(false);
        auto context = std::exchange(Active, nullptr);
        if (context) {
            context->Shutdown();
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
    Config MonitorConfig;
    std::shared_ptr<std::atomic_uint64_t> Sequence = std::make_shared<std::atomic_uint64_t>(std::uint64_t{0});
    std::atomic_bool Running = false;
    mutable std::mutex LifecycleMutex;
    std::shared_ptr<RunContext> Active;
    std::shared_ptr<RunContext> Retiring;
};

ResourcePressureMonitor::ResourcePressureMonitor(Callback callback, Config config)
    : m_impl(std::make_unique<Impl>(std::move(callback), config)) {}

ResourcePressureMonitor::~ResourcePressureMonitor() {
    Stop();
}

bool ResourcePressureMonitor::Start() noexcept {
    return m_impl->Start();
}

void ResourcePressureMonitor::Stop() noexcept {
    m_impl->Stop();
}

bool ResourcePressureMonitor::IsRunning() const noexcept {
    return m_impl->IsRunning();
}
