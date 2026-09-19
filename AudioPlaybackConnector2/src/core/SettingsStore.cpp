#include <pch.h>

#include <core/SettingsStore.hpp>
#include <core/SettingsCodec.hpp>

#include <core/SettingsLimits.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>
#include <util/Util.hpp>

#include <condition_variable>
#include <deque>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Persistence Policy ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

constexpr auto c_debounceDelay = std::chrono::milliseconds(300);
constexpr auto c_maxRetryDelay = std::chrono::minutes(5);

[[nodiscard]] std::chrono::milliseconds RetryDelay(unsigned int failures) noexcept {
    const auto exponent = std::min(failures == 0 ? 0U : failures - 1, 10U);
    const auto milliseconds = std::chrono::milliseconds(1000) * (1U << exponent);
    return std::min(milliseconds, std::chrono::duration_cast<std::chrono::milliseconds>(c_maxRetryDelay));
}

bool BackupUnreadableSettingsFile(std::filesystem::path const& path, util::LogSink const& log) noexcept {
    try {
        if (path.empty() || !std::filesystem::exists(path)) return false;
        auto backup = path;
        backup += L".corrupt.bak";
        for (std::uint64_t suffix = 1; std::filesystem::exists(backup); ++suffix) {
            backup = path;
            backup += std::format(L".corrupt.{}.bak", suffix);
        }
        if (!MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            log.Trace(L"[SettingsStore] ERROR: failed to preserve corrupt file: {0}", path.wstring());
            return false;
        }
        return true;
    } catch (std::exception const& exception) {
        log.Trace(L"[SettingsStore] ERROR: corrupt-file preservation failed: {0}", util::Utf8ToUtf16(exception.what()));
    } catch (...) {
        log.Trace(L"[SettingsStore] ERROR: corrupt-file preservation failed");
    }
    return false;
}

class FilesystemSettingsStoreStorage final : public SettingsStoreStorage {
    util::LogSink log;

public:
    explicit FilesystemSettingsStoreStorage(util::LogSink sink) : log(std::move(sink)) {}

    std::optional<std::string> Read(std::filesystem::path const& path) override {
        wil::unique_hfile file(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            auto const error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
            throw std::runtime_error("settings file could not be opened");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) > apc::limits::c_maxSettingsFileBytes) {
            throw std::runtime_error("settings file is oversized");
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            const auto remaining = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, MAXDWORD));
            if (!ReadFile(file.get(), bytes.data() + offset, remaining, &read, nullptr) || read == 0) {
                throw std::runtime_error("settings file read failed");
            }
            offset += read;
        }
        return bytes;
    }

    bool WriteAtomically(std::filesystem::path const& path, std::string_view bytes) override {
        const auto temporaryPath = std::filesystem::path(path.wstring() + L".tmp");
        auto cleanup = wil::scope_exit([&] { DeleteFileW(temporaryPath.c_str()); });
        wil::unique_hfile file(CreateFileW(
            temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return false;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            const auto remaining = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, MAXDWORD));
            if (!WriteFile(file.get(), bytes.data() + offset, remaining, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
        if (!FlushFileBuffers(file.get())) return false;
        file.reset();
        if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return false;
        cleanup.release();
        return true;
    }

    bool PreserveCorrupt(std::filesystem::path const& path) noexcept override {
        return BackupUnreadableSettingsFile(path, log);
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// System Clock and Wakeup ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class SystemSettingsWakeup final : public SettingsStoreWakeup {
public:
    Clock::time_point Now() const noexcept override { return Clock::now(); }
    std::uint64_t Version() const noexcept override {
        std::scoped_lock lock(m_mutex);
        return m_version;
    }
    void Notify() noexcept override {
        {
            std::scoped_lock lock(m_mutex);
            ++m_version;
        }
        m_changed.notify_one();
    }
    void Wait(std::uint64_t version, std::optional<Clock::time_point> deadline) noexcept override {
        std::unique_lock lock(m_mutex);
        auto changed = [&] { return version != m_version; };
        if (deadline)
            m_changed.wait_until(lock, *deadline, changed);
        else
            m_changed.wait(lock, changed);
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::uint64_t m_version = 0;
};

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings State and Persistence ////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct SettingsStore::Impl final : std::enable_shared_from_this<SettingsStore::Impl> {
    using DataSnapshot = std::shared_ptr<const SettingsData>;
    static_assert(std::is_nothrow_copy_assignable_v<DataSnapshot>);

    explicit Impl(std::filesystem::path directory,
                  std::shared_ptr<SettingsStoreStorage> persistenceStorage,
                  std::shared_ptr<SettingsStoreWakeup> persistenceWakeup,
                  util::LogSink logger)
        : log(std::move(logger)), persistenceDirectory(std::move(directory)), storage(std::move(persistenceStorage)),
          wakeup(persistenceWakeup ? std::move(persistenceWakeup) : std::make_shared<SystemSettingsWakeup>()) {
        if (!storage) storage = std::make_shared<FilesystemSettingsStoreStorage>(log);
    }

    util::LogSink log;
    std::mutex mutex;
    std::mutex publicationMutex;
    std::condition_variable publicationChanged;
    std::condition_variable shutdownChanged;
    std::condition_variable changed;
    // The store and an admitted writer share immutable data. Replacing the
    // current revision cannot change or destroy the writer's captured revision.
    DataSnapshot data = std::make_shared<const SettingsData>();
    std::uint64_t revision = 0;
    std::uint64_t persistedRevision = 0;
    std::uint64_t nextSubscriptionId = 0;
    struct SubscriptionState {
        explicit SubscriptionState(SnapshotCallback callback) : Callback(std::move(callback)) {}

        bool TryBeginCallback() noexcept {
            std::scoped_lock lock(Mutex);
            if (!IsActive) return false;
            IsExecuting = true;
            ExecutingThread = std::this_thread::get_id();
            return true;
        }

        void Deactivate() noexcept {
            std::scoped_lock lock(Mutex);
            IsActive = false;
        }

        void CompleteCallback() noexcept {
            {
                std::scoped_lock lock(Mutex);
                IsExecuting = false;
                ExecutingThread = {};
            }
            Drained.notify_all();
        }

        void DeactivateAndWait() noexcept {
            std::unique_lock lock(Mutex);
            IsActive = false;
            // A callback cannot wait for itself to return. Its inactive state still fences every later
            // publication; CompleteCallback releases concurrent Reset or shutdown callers.
            if (IsExecuting && ExecutingThread == std::this_thread::get_id()) return;
            Drained.wait(lock, [&] { return !IsExecuting; });
        }

        SnapshotCallback Callback;
        std::mutex Mutex;
        std::condition_variable Drained;
        bool IsActive = true;
        bool IsExecuting = false;
        std::thread::id ExecutingThread;
    };
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;
    struct SubscriptionEntry {
        SubscriptionStatePtr State;
    };
    std::unordered_map<std::uint64_t, SubscriptionEntry> subscriptions;
    struct Publication {
        SettingsSnapshot Snapshot;
        std::vector<SubscriptionStatePtr> Subscriptions;
    };
    std::deque<Publication> pendingPublications;
    bool publishing = false;
    std::thread::id publisherThread;
    std::filesystem::path persistenceDirectory;
    std::shared_ptr<SettingsStoreStorage> storage;
    const std::shared_ptr<SettingsStoreWakeup> wakeup;
    bool timerArmed = false;
    std::chrono::steady_clock::time_point due{};
    bool writerActive = false;
    bool closing = false;
    bool shutdownRequested = false;
    bool shutdownCoreComplete = false;
    bool shutdownResult = false;
    bool discardRequested = false;
    bool workerFinished = false;
    bool workerReady = false;
    bool workerFlushResult = false;
    unsigned int shutdownAttempts = 3;
    bool workerStartKnown = false;
    bool loadClaimed = false;
    bool loadActive = false;
    bool preservationFailed = false;
    unsigned int failures = 0;
    std::jthread worker;

    // Called only after releasing the store lock. The worker waits on its own
    // versioned signal; synchronous callers retain the store condition variable.
    void NotifyChanged() noexcept {
        changed.notify_all();
        wakeup->Notify();
    }

    void CompleteLoadWithoutCommit(bool originalPreserved = true) noexcept {
        {
            std::scoped_lock lock(mutex);
            loadActive = false;
            preservationFailed = !originalPreserved;
        }
        NotifyChanged();
    }

    [[nodiscard]] std::filesystem::path Path() const {
        if (!persistenceDirectory.empty()) return persistenceDirectory / L"AudioPlaybackConnector2.json";
        try {
            return std::filesystem::path(
                       std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
                   L"AudioPlaybackConnector2.json";
        } catch (...) {
            wchar_t* localAppData = nullptr;
            const auto result = _wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA");
            const auto cleanup = wil::scope_exit([&] { free(localAppData); });
            if (result == 0 && localAppData && *localAppData) {
                auto directory = std::filesystem::path(localAppData) / L"AudioPlaybackConnector2";
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                if (!error) return directory / L"AudioPlaybackConnector2.json";
            }
        }
        return util::GetModuleFsPath(GetModuleHandleW(nullptr)).remove_filename() / L"AudioPlaybackConnector2.json";
    }

    [[nodiscard]] bool Write(SettingsData const& snapshot) noexcept {
        try {
            const auto utf8 = apc::settings::Encode(snapshot);
            return storage->WriteAtomically(Path(), utf8);
        } catch (std::exception const& exception) {
            log.Trace(L"[SettingsStore] write failed: {0}", util::Utf8ToUtf16(exception.what()));
        } catch (...) {
            log.Trace(L"[SettingsStore] write failed");
        }
        return false;
    }

    void CompleteWriteLocked(std::uint64_t capturedRevision,
                             bool succeeded,
                             SettingsStoreWakeup::Clock::time_point now) noexcept {
        writerActive = false;
        if (shutdownRequested) return;
        if (succeeded) {
            persistedRevision = std::max(persistedRevision, capturedRevision);
            failures = 0;
            if (revision != persistedRevision && !shutdownRequested && !closing) {
                timerArmed = true;
                due = now + c_debounceDelay;
            }
        } else if (!shutdownRequested && !closing) {
            failures = std::min(failures + 1, 10U);
            timerArmed = true;
            due = now + RetryDelay(failures);
        }
    }

    void CompleteWrite(std::uint64_t capturedRevision, bool succeeded) noexcept {
        const auto now = wakeup->Now();
        {
            std::scoped_lock lock(mutex);
            CompleteWriteLocked(capturedRevision, succeeded, now);
        }
        NotifyChanged();
    }

    [[nodiscard]] bool FlushSynchronously(unsigned int maximumAttempts) noexcept {
        util::RuntimeApartment apartment;
        if (!apartment.Ready()) return false;
        maximumAttempts = std::max(maximumAttempts, 1U);
        for (unsigned int attempt = 0; attempt < maximumAttempts; ++attempt) {
            DataSnapshot snapshot;
            std::uint64_t capturedRevision = 0;
            {
                std::unique_lock lock(mutex);
                timerArmed = false;
                changed.notify_all();
                changed.wait(lock, [&] { return shutdownRequested || (!writerActive && !loadActive); });
                if (shutdownRequested || preservationFailed) return false;
                if (discardRequested || revision == persistedRevision) return true;
                writerActive = true;
                snapshot = data;
                capturedRevision = revision;
            }
            const auto succeeded = Write(*snapshot);
            CompleteWrite(capturedRevision, succeeded);
            if (!succeeded) continue;
        }
        std::scoped_lock lock(mutex);
        return revision == persistedRevision;
    }

    void Worker(std::stop_token stopToken) noexcept {
        const auto finish = wil::scope_exit([&] {
            {
                std::scoped_lock lock(mutex);
                workerFinished = true;
            }
            changed.notify_all();
        });
        util::RuntimeApartment apartment;
        {
            std::scoped_lock lock(mutex);
            workerReady = apartment.Ready();
            workerStartKnown = true;
        }
        changed.notify_all();
        if (!apartment.Ready()) return;

        while (!stopToken.stop_requested()) {
            // Capture the signal before inspecting state. A concurrent change
            // then invalidates this version even if Wait has not started yet.
            const auto version = wakeup->Version();
            const auto now = wakeup->Now();
            std::optional<SettingsStoreWakeup::Clock::time_point> deadline;
            DataSnapshot snapshot;
            std::uint64_t capturedRevision = 0;
            {
                std::unique_lock lock(mutex);
                if (shutdownRequested) return;
                if (closing) {
                    const auto attempts = shutdownAttempts;
                    const auto discard = discardRequested;
                    lock.unlock();
                    const auto flushed = discard || FlushSynchronously(attempts);
                    lock.lock();
                    workerFlushResult = flushed;
                    return;
                }
                if (timerArmed && !loadActive && !writerActive && !discardRequested && revision != persistedRevision) {
                    if (now < due) {
                        deadline = due;
                    } else {
                        writerActive = true;
                        timerArmed = false;
                        snapshot = data;
                        capturedRevision = revision;
                    }
                }
            }
            if (snapshot) {
                CompleteWrite(capturedRevision, Write(*snapshot));
            } else {
                wakeup->Wait(version, deadline);
            }
        }
    }
    void EnqueuePublicationWithLockHeld(SettingsSnapshot snapshot,
                                        std::vector<SubscriptionStatePtr> subscriptionStates) {
        pendingPublications.push_back({std::move(snapshot), std::move(subscriptionStates)});
    }

    void DeactivateSubscriptionsLocked() noexcept {
        for (auto const& [_, entry] : subscriptions)
            entry.State->Deactivate();
    }

    void DrainPublications() noexcept {
        // A callback may destroy the public SettingsStore. Hold the implementation through the complete
        // no-lock drain so returning from that callback never resumes into a destroyed Impl.
        const auto lifetime = shared_from_this();
        static_cast<void>(lifetime);
        {
            std::scoped_lock lock(publicationMutex);
            if (publishing) return;
            publishing = true;
            publisherThread = std::this_thread::get_id();
        }
        while (true) {
            Publication publication;
            {
                std::scoped_lock lock(publicationMutex);
                if (pendingPublications.empty()) {
                    publishing = false;
                    publisherThread = {};
                    publicationChanged.notify_all();
                    return;
                }
                publication = std::move(pendingPublications.front());
                pendingPublications.pop_front();
            }
            for (auto const& subscription : publication.Subscriptions) {
                if (!subscription->TryBeginCallback()) continue;
                try {
                    subscription->Callback(publication.Snapshot);
                } catch (...) {
                    log.Trace(L"[SettingsStore] subscriber threw");
                }
                subscription->CompleteCallback();
            }
        }
    }

    [[nodiscard]] bool WaitForPublicationDrain(std::chrono::steady_clock::time_point deadline) noexcept {
        std::unique_lock publicationLock(publicationMutex);
        if (publishing && publisherThread == std::this_thread::get_id()) return true;
        return publicationChanged.wait_until(publicationLock, deadline, [&] { return !publishing; });
    }

    template <typename Mutation> [[nodiscard]] SettingsMutationResult Commit(Mutation&& mutation) {
        // Callbacks may destroy SettingsStore while DrainPublications runs.
        const auto lifetime = shared_from_this();
        static_cast<void>(lifetime);
        std::vector<SubscriptionStatePtr> subscriptionsToNotify;
        SettingsSnapshot snapshot;
        std::uint64_t committedRevision = 0;
        for (;;) {
            const auto now = wakeup->Now();
            std::unique_lock lock(mutex);
            if (loadActive && !closing && !shutdownRequested) {
                changed.wait(lock, [&] { return closing || shutdownRequested || !loadActive; });
                continue;
            }
            if (closing || shutdownRequested || preservationFailed) return {SettingsMutationStatus::Rejected, revision};
            if (revision == std::numeric_limits<std::uint64_t>::max())
                return {SettingsMutationStatus::Rejected, revision};

            // Keep the drainer from observing a queued publication until the no-throw live-state move below
            // completes. Store code takes mutex before publicationMutex whenever it needs both; publication
            // draining and subscriber callbacks never hold either lock while invoking external code.
            std::unique_lock publicationLock(publicationMutex, std::defer_lock);
            if (!subscriptions.empty()) publicationLock.lock();

            // Stage every operation that can allocate or throw against a private candidate. The live state,
            // revision, timer, and publication queue are untouched until the candidate and callback list are
            // complete. Enqueuing before the no-throw move commit prevents a subscriber publication from being
            // lost if a later staging operation fails.
            SettingsData candidate = *data;
            if (!mutation(candidate)) return {SettingsMutationStatus::Unchanged, revision};
            auto committedData = std::make_shared<const SettingsData>(std::move(candidate));
            committedRevision = revision + 1;
            if (!subscriptions.empty()) {
                snapshot = {*committedData, committedRevision, committedRevision != persistedRevision};
                subscriptionsToNotify.reserve(subscriptions.size());
                for (auto const& [_, entry] : subscriptions) {
                    subscriptionsToNotify.push_back(entry.State);
                }
                EnqueuePublicationWithLockHeld(std::move(snapshot), std::move(subscriptionsToNotify));
            }

            // The immutable data pointer can be published without allocation. From this point on the
            // commit consists only of no-throw state publication and the notification below is unconditional.
            data = std::move(committedData);
            revision = committedRevision;
            timerArmed = true;
            due = now + c_debounceDelay;
            break;
        }
        NotifyChanged();
        DrainPublications();
        return {SettingsMutationStatus::Applied, committedRevision};
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Subscription Lifecycle ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsStore::Subscription::~Subscription() {
    Reset();
}

SettingsStore::Subscription::Subscription(Subscription&& other) noexcept
    : m_unsubscribe(std::move(other.m_unsubscribe)) {}

SettingsStore::Subscription& SettingsStore::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_unsubscribe = std::move(other.m_unsubscribe);
    return *this;
}

void SettingsStore::Subscription::Reset() noexcept {
    if (!m_unsubscribe) return;
    try {
        m_unsubscribe();
    } catch (...) {
    }
    m_unsubscribe = {};
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsStore::SettingsStore(std::filesystem::path persistenceDirectory,
                             std::shared_ptr<SettingsStoreStorage> storage,
                             std::shared_ptr<SettingsStoreWakeup> wakeup,
                             util::LogSink log)
    : m_impl(std::make_shared<Impl>(
          std::move(persistenceDirectory), std::move(storage), std::move(wakeup), std::move(log))) {
    // Without its persistence executor the store cannot offer a bounded shutdown.
    // Fail construction instead of falling back to storage I/O on a UI or callback thread.
    m_impl->worker = std::jthread([impl = m_impl](std::stop_token stopToken) { impl->Worker(stopToken); });
    std::unique_lock lock(m_impl->mutex);
    m_impl->changed.wait(lock, [&] { return m_impl->workerStartKnown; });
    if (!m_impl->workerReady) {
        lock.unlock();
        m_impl->worker.join();
        throw std::runtime_error("SettingsStore persistence apartment initialization failed");
    }
}

SettingsStore::~SettingsStore() {
    static_cast<void>(Shutdown(SettingsShutdownMode::Flush, 3));
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsSnapshot SettingsStore::Snapshot() const {
    Impl::DataSnapshot data;
    std::uint64_t revision;
    bool dirty;
    {
        std::scoped_lock lock(m_impl->mutex);
        data = m_impl->data;
        revision = m_impl->revision;
        dirty = revision != m_impl->persistedRevision;
    }
    return {*data, revision, dirty};
}

SettingsStore::Subscription SettingsStore::Subscribe(SnapshotCallback callback) {
    if (!callback) return {};
    std::scoped_lock lock(m_impl->mutex);
    if (m_impl->shutdownRequested || m_impl->closing) return {};
    const auto identifier = ++m_impl->nextSubscriptionId;
    auto state = std::make_shared<Impl::SubscriptionState>(std::move(callback));
    m_impl->subscriptions.emplace(identifier, Impl::SubscriptionEntry{state});
    std::weak_ptr weak = m_impl;
    return Subscription([weak, state, identifier] {
        state->DeactivateAndWait();
        if (auto impl = weak.lock()) {
            std::scoped_lock lock(impl->mutex);
            impl->subscriptions.erase(identifier);
        }
    });
}

void SettingsStore::Load() {
    const auto lifetime = m_impl;
    static_cast<void>(lifetime);
    SettingsData loaded;
    {
        std::scoped_lock lock(lifetime->mutex);
        if (lifetime->loadClaimed || lifetime->closing || lifetime->shutdownRequested) return;
        lifetime->loadClaimed = true;
        // Runtime changes own the state once they have committed. A later Load must not race the writer or
        // replace those changes with an older file snapshot.
        if (lifetime->revision != 0) return;
        lifetime->loadActive = true;
    }
    std::filesystem::path path;
    try {
        path = lifetime->Path();
        const auto bytes = lifetime->storage->Read(path);
        if (!bytes) {
            lifetime->CompleteLoadWithoutCommit();
            return;
        }
        loaded = apc::settings::Decode(*bytes);
    } catch (std::exception const& exception) {
        lifetime->log.Trace(L"[SettingsStore] load failed: {0}", util::Utf8ToUtf16(exception.what()));
        lifetime->CompleteLoadWithoutCommit(lifetime->storage->PreserveCorrupt(path));
        return;
    } catch (...) {
        lifetime->log.Trace(L"[SettingsStore] load failed");
        lifetime->CompleteLoadWithoutCommit(lifetime->storage->PreserveCorrupt(path));
        return;
    }
    std::vector<Impl::SubscriptionStatePtr> subscriptionsToNotify;
    SettingsSnapshot snapshot;
    try {
        auto loadedData = std::make_shared<const SettingsData>(std::move(loaded));
        {
            std::scoped_lock lock(lifetime->mutex);
            if (lifetime->shutdownRequested || lifetime->revision != 0) {
                lifetime->loadActive = false;
                lifetime->changed.notify_all();
                return;
            }

            std::unique_lock publicationLock(lifetime->publicationMutex, std::defer_lock);
            const auto shouldPublish = !lifetime->closing && !lifetime->subscriptions.empty();
            if (shouldPublish) publicationLock.lock();
            if (shouldPublish) {
                snapshot = {*loadedData, 1, false};
                subscriptionsToNotify.reserve(lifetime->subscriptions.size());
                for (auto const& [_, entry] : lifetime->subscriptions) {
                    subscriptionsToNotify.push_back(entry.State);
                }
                lifetime->EnqueuePublicationWithLockHeld(std::move(snapshot), std::move(subscriptionsToNotify));
            }

            lifetime->data = std::move(loadedData);
            lifetime->revision = 1;
            lifetime->persistedRevision = 1;
            lifetime->loadActive = false;
        }
        lifetime->NotifyChanged();
    } catch (...) {
        lifetime->CompleteLoadWithoutCommit();
        throw;
    }
    lifetime->DrainPublications();
}

namespace {
DeviceSettings* FindDevice(SettingsData& data, std::wstring_view id) {
    const auto it = std::ranges::find(data.Devices, id, &DeviceSettings::Id);
    return it == data.Devices.end() ? nullptr : &*it;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings Mutations ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsMutationResult SettingsStore::SetGlobalConnectOnStartup(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.GlobalConnectOnStartup, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetGlobalReconnectOnConnectionLoss(bool enabled) {
    return m_impl->Commit(
        [=](auto& data) { return std::exchange(data.GlobalReconnectOnConnectionLoss, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetAllowIncomingConnections(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.AllowIncomingConnections, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetStartWithWindows(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.StartWithWindows, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetShowNotifications(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.ShowNotifications, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetUseSystemBackdropEffects(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.UseSystemBackdropEffects, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetLanguage(std::wstring_view language) {
    if (!apc::limits::IsSupportedLanguage(language)) return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit(
        [language = std::wstring(language)](auto& data) { return std::exchange(data.Language, language) != language; });
}
SettingsMutationResult SettingsStore::SetPrivacyModeEnabled(bool enabled) {
    return m_impl->Commit([=](auto& data) { return std::exchange(data.PrivacyModeEnabled, enabled) != enabled; });
}
SettingsMutationResult SettingsStore::SetSettingsWindowBounds(std::optional<PersistedWindowBounds> bounds) {
    if (bounds && (bounds->Width <= 0 || bounds->Height <= 0 || bounds->Dpi < apc::limits::c_minWindowDpi ||
                   bounds->Dpi > apc::limits::c_maxWindowDpi))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([bounds = std::move(bounds)](auto& data) {
        return std::exchange(data.SettingsWindowBounds, bounds) != bounds;
    });
}
SettingsMutationResult SettingsStore::RememberDevice(std::wstring_view deviceId, std::wstring_view deviceName) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters) ||
        !apc::limits::IsBoundedUtf16(deviceName, apc::limits::c_maxDeviceNameCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([deviceId = std::wstring(deviceId), deviceName = std::wstring(deviceName)](auto& data) {
        if (FindDevice(data, deviceId) || data.Devices.size() == apc::limits::c_maxPersistedDeviceCount) return false;
        // Configuring a discovered device must not mark it as connected or change the last-device target.
        data.Devices.push_back(
            {deviceId, deviceName, L"", data.GlobalConnectOnStartup, data.GlobalReconnectOnConnectionLoss});
        return true;
    });
}

SettingsMutationResult SettingsStore::SetDeviceConnectOnStartup(std::wstring_view deviceId, bool enabled) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([=](auto& data) {
        auto* device = FindDevice(data, deviceId);
        if (!device) return false;
        return std::exchange(device->ConnectOnStartup, enabled) != enabled;
    });
}
SettingsMutationResult SettingsStore::SetDeviceReconnectOnConnectionLoss(std::wstring_view deviceId, bool enabled) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([=](auto& data) {
        auto* device = FindDevice(data, deviceId);
        if (!device) return false;
        return std::exchange(device->ReconnectOnConnectionLoss, enabled) != enabled;
    });
}
DeviceAliasResult SettingsStore::SetDeviceAlias(std::wstring_view deviceId,
                                                std::wstring_view alias,
                                                std::optional<std::wstring> deviceName) {
    DeviceAliasResult result;
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters) ||
        !apc::limits::IsBoundedUtf16(alias, apc::limits::c_maxDeviceAliasCharacters) ||
        (deviceName && !apc::limits::IsBoundedUtf16(*deviceName, apc::limits::c_maxDeviceNameCharacters)))
        result.Mutation = {SettingsMutationStatus::Rejected, Snapshot().Revision};
    else
        result.Mutation = m_impl->Commit([&result,
                                          deviceId = std::wstring(deviceId),
                                          alias = std::wstring(alias),
                                          deviceName = std::move(deviceName)](auto& data) {
            auto* device = FindDevice(data, deviceId);
            if (!device) {
                if (alias.empty() || data.Devices.size() == apc::limits::c_maxPersistedDeviceCount) return false;
                data.Devices.push_back({deviceId,
                                        deviceName.value_or(L""),
                                        alias,
                                        data.GlobalConnectOnStartup,
                                        data.GlobalReconnectOnConnectionLoss});
                result.DeviceExists = true;
                return true;
            }
            result.DeviceExists = true;
            const auto shouldUpdateName = deviceName && !deviceName->empty() && device->Name != *deviceName;
            const auto changed = device->Alias != alias || shouldUpdateName;
            device->Alias = alias;
            if (shouldUpdateName) device->Name = *deviceName;
            return changed;
        });
    return result;
}
SettingsMutationResult SettingsStore::SetDefaultDevice(std::wstring_view deviceId) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([deviceId = std::wstring(deviceId)](auto& data) {
        const auto changed =
            data.DefaultDevice != DefaultDeviceMode::SpecificDevice || data.DefaultDeviceId != deviceId;
        data.DefaultDevice = DefaultDeviceMode::SpecificDevice;
        data.DefaultDeviceId = deviceId;
        return changed;
    });
}
SettingsMutationResult SettingsStore::ClearDefaultDevice() {
    return m_impl->Commit([](auto& data) {
        const auto changed = data.DefaultDevice != DefaultDeviceMode::LastConnected || !data.DefaultDeviceId.empty();
        data.DefaultDevice = DefaultDeviceMode::LastConnected;
        data.DefaultDeviceId.clear();
        return changed;
    });
}
SettingsMutationResult SettingsStore::ForgetDevice(std::wstring_view deviceId) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([deviceId = std::wstring(deviceId)](auto& data) {
        const auto before = data.Devices.size() + data.LastConnectedIds.size();
        const auto defaultWasRemoved = data.DefaultDeviceId == deviceId;
        std::erase_if(data.Devices, [&](auto const& device) { return device.Id == deviceId; });
        std::erase(data.LastConnectedIds, deviceId);
        if (defaultWasRemoved) {
            data.DefaultDevice = DefaultDeviceMode::LastConnected;
            data.DefaultDeviceId.clear();
        }
        return defaultWasRemoved || before != data.Devices.size() + data.LastConnectedIds.size();
    });
}
SettingsMutationResult SettingsStore::RecordConnectedDevice(std::wstring_view deviceId, std::wstring_view deviceName) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters) ||
        !apc::limits::IsBoundedUtf16(deviceName, apc::limits::c_maxDeviceNameCharacters)) {
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    }
    return m_impl->Commit([deviceId = std::wstring(deviceId), deviceName = std::wstring(deviceName)](auto& data) {
        auto* device = FindDevice(data, deviceId);
        bool changed = false;
        if (!device && data.Devices.size() < apc::limits::c_maxPersistedDeviceCount) {
            data.Devices.push_back(
                {deviceId, deviceName, L"", data.GlobalConnectOnStartup, data.GlobalReconnectOnConnectionLoss});
            changed = true;
        } else if (device && !deviceName.empty() && device->Name != deviceName) {
            device->Name = deviceName;
            changed = true;
        }
        const auto before = data.LastConnectedIds;
        std::erase(data.LastConnectedIds, deviceId);
        data.LastConnectedIds.insert(data.LastConnectedIds.begin(), deviceId);
        if (data.LastConnectedIds.size() > apc::limits::c_maxPersistedDeviceCount) data.LastConnectedIds.pop_back();
        return changed || data.LastConnectedIds != before;
    });
}
/*------------------------------------------------------------------------------------------------------------*/
/*//////// Flush and Shutdown ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SettingsStore::FlushNow(unsigned int maximumAttempts) noexcept {
    const auto impl = m_impl;
    return impl->FlushSynchronously(maximumAttempts);
}

bool SettingsStore::Shutdown(SettingsShutdownMode mode,
                             unsigned int maximumAttempts,
                             std::chrono::milliseconds timeBudget) noexcept {
    const auto impl = m_impl;
    const auto deadline = std::chrono::steady_clock::now() + std::max(timeBudget, std::chrono::milliseconds::zero());
    bool workerFinished = false;
    bool result = false;
    decltype(impl->subscriptions) retiredSubscriptions;
    {
        std::unique_lock lock(impl->mutex);
        if (impl->closing) {
            if (!impl->shutdownChanged.wait_until(lock, deadline, [&] { return impl->shutdownCoreComplete; }))
                return false;
            result = impl->shutdownResult;
            lock.unlock();
            return result && impl->WaitForPublicationDrain(deadline);
        }
        impl->closing = true;
        impl->timerArmed = false;
        impl->shutdownAttempts = std::max(maximumAttempts, 1U);
        impl->discardRequested = mode == SettingsShutdownMode::DiscardStartupFailure;
        impl->DeactivateSubscriptionsLocked();
        retiredSubscriptions = std::move(impl->subscriptions);
        lock.unlock();
        impl->NotifyChanged();
        lock.lock();

        // Only the persistence worker performs the final write. This caller never enters storage I/O.
        const auto drained = impl->changed.wait_until(
            lock, deadline, [&] { return impl->workerFinished && !impl->writerActive && !impl->loadActive; });
        result = drained && impl->workerFlushResult;
        workerFinished = impl->workerFinished;
        // Fence late load/write completions even when their platform call cannot be interrupted.
        impl->shutdownRequested = true;
        impl->timerArmed = false;
    }
    impl->NotifyChanged();
    impl->worker.request_stop();
    if (workerFinished) {
        impl->worker.join();
    } else {
        // The worker retains Impl, including storage and every value used after this boundary.
        // Detaching releases only the thread handle; worker exit releases the retained state.
        // No callback captures the facade. The shutdown fence rejects new work and late state commits.
        impl->worker.detach();
    }
    {
        std::scoped_lock lock(impl->mutex);
        impl->shutdownResult = result;
        impl->shutdownCoreComplete = true;
    }
    impl->shutdownChanged.notify_all();
    const auto publicationsDrained = impl->WaitForPublicationDrain(deadline);
    if (!result || !publicationsDrained)
        impl->log.Trace(L"[SettingsStore] shutdown incomplete: persistence or publication did not drain");
    return result && publicationsDrained;
}
