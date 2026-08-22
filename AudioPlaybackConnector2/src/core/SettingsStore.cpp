#include <pch.h>

#include <core/SettingsStore.hpp>

#include <core/SettingsLimits.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>
#include <util/Util.hpp>

#include <condition_variable>
#include <deque>
#include <limits>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr auto c_debounceDelay = std::chrono::milliseconds(300);
constexpr auto c_maxRetryDelay = std::chrono::minutes(5);

[[nodiscard]] std::wstring BoundedString(winrt::hstring const& value, std::size_t limit) {
    return apc::limits::TruncateUtf16(std::wstring_view(value), limit);
}

[[nodiscard]] bool IsPersistable(SettingsData const& data) {
    if (data.Devices.size() > apc::limits::c_maxPersistedDeviceCount ||
        data.LastConnectedIds.size() > apc::limits::c_maxPersistedDeviceCount ||
        !apc::limits::IsSupportedLanguage(data.Language) ||
        !apc::limits::IsBoundedUtf16(data.LastNotifiedUpdateVersion, apc::limits::c_maxVersionCharacters) ||
        !apc::limits::IsBoundedUtf16(data.DefaultDeviceId, apc::limits::c_maxDeviceIdCharacters) ||
        ((data.DefaultDevice == DefaultDeviceMode::SpecificDevice) != !data.DefaultDeviceId.empty())) {
        return false;
    }
    if (data.SettingsWindowBounds && (data.SettingsWindowBounds->Width <= 0 || data.SettingsWindowBounds->Height <= 0 ||
                                      data.SettingsWindowBounds->Dpi < apc::limits::c_minWindowDpi ||
                                      data.SettingsWindowBounds->Dpi > apc::limits::c_maxWindowDpi)) {
        return false;
    }
    std::unordered_set<std::wstring_view> deviceIds;
    std::unordered_set<std::wstring_view> connectedIds;
    for (auto const& device : data.Devices) {
        if (device.Id.empty() || !apc::limits::IsBoundedUtf16(device.Id, apc::limits::c_maxDeviceIdCharacters) ||
            !apc::limits::IsBoundedUtf16(device.Name, apc::limits::c_maxDeviceNameCharacters) ||
            !apc::limits::IsBoundedUtf16(device.Alias, apc::limits::c_maxDeviceAliasCharacters) ||
            !deviceIds.insert(device.Id).second) {
            return false;
        }
    }
    for (auto const& id : data.LastConnectedIds) {
        if (id.empty() || !apc::limits::IsBoundedUtf16(id, apc::limits::c_maxDeviceIdCharacters) ||
            !connectedIds.insert(id).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
GetOptionalBoolean(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, bool fallback) {
    if (!json.HasKey(key)) return fallback;
    const auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean ? value.GetBoolean() : fallback;
}

[[nodiscard]] winrt::hstring GetOptionalString(winrt::Windows::Data::Json::JsonObject const& json,
                                               winrt::hstring const& key,
                                               winrt::hstring const& fallback) {
    if (!json.HasKey(key)) return fallback;
    const auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String ? value.GetString() : fallback;
}

[[nodiscard]] std::int64_t
GetOptionalInt64(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, std::int64_t fallback) {
    if (!json.HasKey(key)) return fallback;
    const auto value = json.Lookup(key);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return fallback;
    const auto number = value.GetNumber();
    constexpr double c_int64Min = -9223372036854775808.0;
    constexpr double c_int64ExclusiveMax = 9223372036854775808.0;
    if (!std::isfinite(number) || std::trunc(number) != number || number < c_int64Min || number >= c_int64ExclusiveMax)
        return fallback;
    return static_cast<std::int64_t>(number);
}

[[nodiscard]] std::int32_t
GetOptionalInt32(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, std::int32_t fallback) {
    if (!json.HasKey(key)) return fallback;
    const auto value = json.Lookup(key);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return fallback;
    const auto number = value.GetNumber();
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        number > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return fallback;
    }
    return static_cast<std::int32_t>(number);
}

[[nodiscard]] winrt::Windows::Data::Json::JsonObject
GetOptionalObject(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key) {
    if (!json.HasKey(key)) return nullptr;
    const auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object ? value.GetObject() : nullptr;
}

[[nodiscard]] winrt::Windows::Data::Json::JsonArray GetOptionalArray(winrt::Windows::Data::Json::JsonObject const& json,
                                                                     winrt::hstring const& key) {
    if (!json.HasKey(key)) return nullptr;
    const auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array ? value.GetArray() : nullptr;
}

[[nodiscard]] DefaultDeviceMode ParseDefaultDeviceMode(std::wstring_view value) noexcept {
    return value == L"specificDevice" ? DefaultDeviceMode::SpecificDevice : DefaultDeviceMode::LastConnected;
}

[[nodiscard]] std::wstring_view SerializeDefaultDeviceMode(DefaultDeviceMode mode) noexcept {
    return mode == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
}

[[nodiscard]] std::chrono::milliseconds RetryDelay(unsigned int failures) noexcept {
    const auto exponent = std::min(failures == 0 ? 0U : failures - 1, 10U);
    const auto milliseconds = std::chrono::milliseconds(1000) * (1U << exponent);
    return std::min(milliseconds, std::chrono::duration_cast<std::chrono::milliseconds>(c_maxRetryDelay));
}

void BackupUnreadableSettingsFile(std::filesystem::path const& path) noexcept {
    try {
        if (path.empty() || !std::filesystem::exists(path)) return;
        auto backup = path;
        backup += L".corrupt.bak";
        for (std::uint64_t suffix = 1; std::filesystem::exists(backup); ++suffix) {
            backup = path;
            backup += std::format(L".corrupt.{}.bak", suffix);
        }
        if (!MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            DebugTrace(L"[SettingsStore] ERROR: failed to preserve corrupt file: {0}", path.wstring());
        }
    } catch (std::exception const& exception) {
        DebugTrace(L"[SettingsStore] ERROR: corrupt-file preservation failed: {0}",
                   util::Utf8ToUtf16(exception.what()));
    } catch (...) {
        DebugTrace(L"[SettingsStore] ERROR: corrupt-file preservation failed");
    }
}

class FilesystemSettingsStoreStorage final : public SettingsStoreStorage {
public:
    std::optional<std::string> Read(std::filesystem::path const& path) override {
        wil::unique_hfile file(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return std::nullopt;
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

    void PreserveCorrupt(std::filesystem::path const& path) noexcept override { BackupUnreadableSettingsFile(path); }
};

} // namespace

struct SettingsStore::Impl final : std::enable_shared_from_this<SettingsStore::Impl> {
    static_assert(std::is_nothrow_move_assignable_v<SettingsData>);

    explicit Impl(std::filesystem::path directory, std::shared_ptr<SettingsStoreStorage> persistenceStorage)
        : persistenceDirectory(std::move(directory)), storage(std::move(persistenceStorage)) {
        if (!storage) storage = std::make_shared<FilesystemSettingsStoreStorage>();
    }

    std::mutex mutex;
    std::mutex publicationMutex;
    std::condition_variable publicationChanged;
    std::condition_variable shutdownChanged;
    std::condition_variable changed;
    SettingsData data;
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
    bool timerArmed = false;
    std::chrono::steady_clock::time_point due{};
    bool writerActive = false;
    bool closing = false;
    bool shutdownRequested = false;
    bool shutdownCoreComplete = false;
    bool shutdownResult = false;
    bool discardRequested = false;
    bool synchronousFallback = false;
    bool workerStartKnown = false;
    bool loadClaimed = false;
    bool loadActive = false;
    unsigned int failures = 0;
    std::jthread worker;

    [[nodiscard]] SettingsSnapshot SnapshotLocked() const { return {data, revision, revision != persistedRevision}; }

    void CompleteLoadWithoutCommit() noexcept {
        {
            std::scoped_lock lock(mutex);
            loadActive = false;
        }
        changed.notify_all();
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
            if (!IsPersistable(snapshot)) {
                DebugTrace(L"[SettingsStore] ERROR: refusing invalid or oversized settings");
                return false;
            }
            winrt::Windows::Data::Json::JsonObject json;
            json.Insert(L"globalConnectOnStartup",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.GlobalConnectOnStartup));
            json.Insert(
                L"globalReconnectOnConnectionLoss",
                winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.GlobalReconnectOnConnectionLoss));
            json.Insert(L"allowIncomingConnections",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.AllowIncomingConnections));
            json.Insert(L"startWithWindows",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.StartWithWindows));
            json.Insert(L"showNotifications",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.ShowNotifications));
            json.Insert(L"useSystemBackdropEffects",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.UseSystemBackdropEffects));
            json.Insert(L"privacyModeEnabled",
                        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.PrivacyModeEnabled));
            json.Insert(L"language", winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.Language));
            json.Insert(L"lastUpdateCheckUnixSeconds",
                        winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
                            static_cast<double>(snapshot.LastUpdateCheckUnixSeconds)));
            json.Insert(L"lastNotifiedUpdateVersion",
                        winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.LastNotifiedUpdateVersion));
            json.Insert(L"defaultDeviceMode",
                        winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                            winrt::hstring(SerializeDefaultDeviceMode(snapshot.DefaultDevice))));
            json.Insert(L"defaultDeviceId",
                        winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.DefaultDeviceId));
            if (snapshot.SettingsWindowBounds) {
                winrt::Windows::Data::Json::JsonObject bounds;
                bounds.Insert(
                    L"x", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->X));
                bounds.Insert(
                    L"y", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Y));
                bounds.Insert(
                    L"width",
                    winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Width));
                bounds.Insert(
                    L"height",
                    winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Height));
                bounds.Insert(
                    L"dpi",
                    winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Dpi));
                json.Insert(L"settingsWindowBounds", bounds);
            }
            winrt::Windows::Data::Json::JsonArray devices;
            for (auto const& device : snapshot.Devices) {
                winrt::Windows::Data::Json::JsonObject entry;
                entry.Insert(L"id", winrt::Windows::Data::Json::JsonValue::CreateStringValue(device.Id));
                entry.Insert(L"name", winrt::Windows::Data::Json::JsonValue::CreateStringValue(device.Name));
                entry.Insert(L"alias", winrt::Windows::Data::Json::JsonValue::CreateStringValue(device.Alias));
                entry.Insert(L"connectOnStartup",
                             winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(device.ConnectOnStartup));
                entry.Insert(
                    L"reconnectOnConnectionLoss",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(device.ReconnectOnConnectionLoss));
                devices.Append(entry);
            }
            json.Insert(L"devices", devices);
            winrt::Windows::Data::Json::JsonArray lastConnected;
            for (auto const& id : snapshot.LastConnectedIds)
                lastConnected.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(id));
            json.Insert(L"lastConnectedIds", lastConnected);
            const auto utf8 = util::Utf16ToUtf8(json.Stringify());
            if (utf8.size() > apc::limits::c_maxSettingsFileBytes) return false;
            return storage->WriteAtomically(Path(), utf8);
        } catch (std::exception const& exception) {
            DebugTrace(L"[SettingsStore] write failed: {0}", util::Utf8ToUtf16(exception.what()));
        } catch (...) {
            DebugTrace(L"[SettingsStore] write failed");
        }
        return false;
    }

    void CompleteWrite(std::uint64_t capturedRevision, bool succeeded) noexcept {
        std::scoped_lock lock(mutex);
        writerActive = false;
        if (succeeded) {
            persistedRevision = std::max(persistedRevision, capturedRevision);
            failures = 0;
            if (revision != persistedRevision && !shutdownRequested && !closing) {
                timerArmed = true;
                due = std::chrono::steady_clock::now() + c_debounceDelay;
            }
        } else if (!shutdownRequested && !closing) {
            failures = std::min(failures + 1, 10U);
            timerArmed = true;
            due = std::chrono::steady_clock::now() + RetryDelay(failures);
        }
        changed.notify_all();
    }

    [[nodiscard]] bool FlushSynchronously(unsigned int maximumAttempts) noexcept {
        util::RuntimeApartment apartment;
        if (!apartment.Ready()) return false;
        maximumAttempts = std::max(maximumAttempts, 1U);
        for (unsigned int attempt = 0; attempt < maximumAttempts; ++attempt) {
            SettingsData snapshot;
            std::uint64_t capturedRevision = 0;
            {
                std::unique_lock lock(mutex);
                timerArmed = false;
                changed.notify_all();
                changed.wait(lock, [&] { return !writerActive && !loadActive; });
                if (discardRequested || revision == persistedRevision) return true;
                writerActive = true;
                snapshot = data;
                capturedRevision = revision;
            }
            const auto succeeded = Write(snapshot);
            CompleteWrite(capturedRevision, succeeded);
            if (!succeeded) continue;
        }
        std::scoped_lock lock(mutex);
        return revision == persistedRevision;
    }

    void Worker(std::stop_token stopToken) noexcept {
        util::RuntimeApartment apartment;
        if (!apartment.Ready()) {
            std::scoped_lock lock(mutex);
            synchronousFallback = true;
            workerStartKnown = true;
            timerArmed = false;
            changed.notify_all();
            return;
        }
        std::unique_lock lock(mutex);
        workerStartKnown = true;
        changed.notify_all();
        while (!stopToken.stop_requested()) {
            changed.wait(
                lock, [&] { return stopToken.stop_requested() || shutdownRequested || (timerArmed && !loadActive); });
            if (stopToken.stop_requested() || shutdownRequested) return;
            const auto scheduled = due;
            if (changed.wait_until(lock, scheduled, [&] {
                    return stopToken.stop_requested() || shutdownRequested || due != scheduled || loadActive;
                }))
                continue;
            SettingsData snapshot;
            std::uint64_t capturedRevision = 0;
            if (writerActive || revision == persistedRevision || discardRequested) continue;
            writerActive = true;
            timerArmed = false;
            snapshot = data;
            capturedRevision = revision;
            lock.unlock();
            const auto succeeded = Write(snapshot);
            CompleteWrite(capturedRevision, succeeded);
            lock.lock();
        }
    }

    void EnqueuePublicationWithLockHeld(SettingsSnapshot snapshot,
                                        std::vector<SubscriptionStatePtr> subscriptionStates) {
        pendingPublications.push_back({std::move(snapshot), std::move(subscriptionStates)});
    }

    void DeactivateSubscriptionsLocked() noexcept {
        for (auto const& [_, entry] : subscriptions)
            entry.State->Deactivate();
        subscriptions.clear();
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
                    DebugTrace(L"[SettingsStore] subscriber threw");
                }
                subscription->CompleteCallback();
            }
        }
    }

    void WaitForPublicationDrain() noexcept {
        std::unique_lock publicationLock(publicationMutex);
        if (publishing && publisherThread == std::this_thread::get_id()) return;
        publicationChanged.wait(publicationLock, [&] { return !publishing; });
    }

    template <typename Mutation> [[nodiscard]] SettingsMutationResult Commit(Mutation&& mutation) {
        // Callbacks may destroy SettingsStore while DrainPublications runs. Keep the implementation alive
        // through the synchronous-fallback write that follows the drain as well.
        const auto lifetime = shared_from_this();
        static_cast<void>(lifetime);
        std::vector<SubscriptionStatePtr> subscriptionsToNotify;
        SettingsSnapshot snapshot;
        std::uint64_t committedRevision = 0;
        bool useSynchronousFallback = false;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return closing || shutdownRequested || !loadActive; });
            if (closing || shutdownRequested) return {SettingsMutationStatus::Rejected, revision};
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
            SettingsData candidate = data;
            if (!mutation(candidate)) return {SettingsMutationStatus::Unchanged, revision};
            committedRevision = revision + 1;
            if (!subscriptions.empty()) {
                snapshot = {candidate, committedRevision, committedRevision != persistedRevision};
                subscriptionsToNotify.reserve(subscriptions.size());
                for (auto const& [_, entry] : subscriptions) {
                    subscriptionsToNotify.push_back(entry.State);
                }
                EnqueuePublicationWithLockHeld(std::move(snapshot), std::move(subscriptionsToNotify));
            }

            // SettingsData is statically required to have a no-throw move assignment. From this point on the
            // commit consists only of no-throw state publication and the notification below is unconditional.
            data = std::move(candidate);
            revision = committedRevision;
            timerArmed = true;
            due = std::chrono::steady_clock::now() + c_debounceDelay;
            useSynchronousFallback = synchronousFallback;
        }
        changed.notify_all();
        DrainPublications();
        if (useSynchronousFallback) static_cast<void>(FlushSynchronously(3));
        return {SettingsMutationStatus::Applied, committedRevision};
    }
};

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

SettingsStore::SettingsStore(std::filesystem::path persistenceDirectory, std::shared_ptr<SettingsStoreStorage> storage)
    : m_impl(std::make_shared<Impl>(std::move(persistenceDirectory), std::move(storage))) {
    try {
        m_impl->worker = std::jthread([impl = m_impl](std::stop_token stopToken) { impl->Worker(stopToken); });
    } catch (std::exception const& exception) {
        DebugTrace(L"[SettingsStore] worker creation failed; writes will be synchronous: {0}",
                   util::Utf8ToUtf16(exception.what()));
        std::scoped_lock lock(m_impl->mutex);
        m_impl->synchronousFallback = true;
        m_impl->workerStartKnown = true;
    } catch (...) {
        DebugTrace(L"[SettingsStore] worker creation failed; writes will be synchronous");
        std::scoped_lock lock(m_impl->mutex);
        m_impl->synchronousFallback = true;
        m_impl->workerStartKnown = true;
    }
    std::unique_lock lock(m_impl->mutex);
    m_impl->changed.wait(lock, [&] { return m_impl->workerStartKnown; });
}

SettingsStore::~SettingsStore() {
    static_cast<void>(Shutdown(SettingsShutdownMode::Flush, 3));
}

SettingsSnapshot SettingsStore::Snapshot() const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->SnapshotLocked();
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
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->loadClaimed || m_impl->closing || m_impl->shutdownRequested) return;
        m_impl->loadClaimed = true;
        // Runtime changes own the state once they have committed. A later Load must not race the writer or
        // replace those changes with an older file snapshot.
        if (m_impl->revision != 0) return;
        m_impl->loadActive = true;
    }
    std::filesystem::path path;
    try {
        path = m_impl->Path();
        const auto bytes = m_impl->storage->Read(path);
        if (!bytes || bytes->empty()) {
            m_impl->CompleteLoadWithoutCommit();
            return;
        }
        const auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(*bytes));
        const auto legacyGlobal = GetOptionalBoolean(json, L"globalAutoReconnect", false);
        loaded.GlobalConnectOnStartup = GetOptionalBoolean(json, L"globalConnectOnStartup", legacyGlobal);
        loaded.GlobalReconnectOnConnectionLoss =
            GetOptionalBoolean(json, L"globalReconnectOnConnectionLoss", legacyGlobal);
        loaded.AllowIncomingConnections = GetOptionalBoolean(json, L"allowIncomingConnections", false);
        loaded.StartWithWindows = GetOptionalBoolean(json, L"startWithWindows", false);
        loaded.ShowNotifications = GetOptionalBoolean(json, L"showNotifications", true);
        loaded.UseSystemBackdropEffects = GetOptionalBoolean(json, L"useSystemBackdropEffects", true);
        loaded.PrivacyModeEnabled = GetOptionalBoolean(json, L"privacyModeEnabled", false);
        loaded.LastUpdateCheckUnixSeconds = GetOptionalInt64(json, L"lastUpdateCheckUnixSeconds", 0);
        loaded.LastNotifiedUpdateVersion = BoundedString(GetOptionalString(json, L"lastNotifiedUpdateVersion", L""),
                                                         apc::limits::c_maxVersionCharacters);
        const auto language = GetOptionalString(json, L"language", L"system");
        loaded.Language = apc::limits::IsSupportedLanguage(language) ? std::wstring(language) : L"system";
        loaded.DefaultDevice = ParseDefaultDeviceMode(GetOptionalString(json, L"defaultDeviceMode", L""));
        loaded.DefaultDeviceId = GetOptionalString(json, L"defaultDeviceId", L"");
        if (loaded.DefaultDevice != DefaultDeviceMode::SpecificDevice || loaded.DefaultDeviceId.empty() ||
            !apc::limits::IsBoundedUtf16(loaded.DefaultDeviceId, apc::limits::c_maxDeviceIdCharacters)) {
            loaded.DefaultDevice = DefaultDeviceMode::LastConnected;
            loaded.DefaultDeviceId.clear();
        }
        if (auto boundsJson = GetOptionalObject(json, L"settingsWindowBounds")) {
            const auto persistedDpi = GetOptionalInt32(boundsJson, L"dpi", USER_DEFAULT_SCREEN_DPI);
            PersistedWindowBounds bounds{
                GetOptionalInt32(boundsJson, L"x", 0),
                GetOptionalInt32(boundsJson, L"y", 0),
                GetOptionalInt32(boundsJson, L"width", 0),
                GetOptionalInt32(boundsJson, L"height", 0),
                static_cast<std::uint32_t>(persistedDpi > 0 ? persistedDpi : USER_DEFAULT_SCREEN_DPI)};
            if (bounds.Width > 0 && bounds.Height > 0 && bounds.Dpi >= apc::limits::c_minWindowDpi &&
                bounds.Dpi <= apc::limits::c_maxWindowDpi)
                loaded.SettingsWindowBounds = bounds;
        }
        std::unordered_set<std::wstring> deviceIds;
        if (auto devices = GetOptionalArray(json, L"devices"))
            for (auto value : devices) {
                if (loaded.Devices.size() == apc::limits::c_maxPersistedDeviceCount) break;
                if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
                try {
                    const auto entry = value.GetObject();
                    DeviceSettings device;
                    device.Id = GetOptionalString(entry, L"id", L"");
                    if (device.Id.empty() ||
                        !apc::limits::IsBoundedUtf16(device.Id, apc::limits::c_maxDeviceIdCharacters) ||
                        !deviceIds.insert(device.Id).second)
                        continue;
                    device.Name =
                        BoundedString(GetOptionalString(entry, L"name", L""), apc::limits::c_maxDeviceNameCharacters);
                    device.Alias =
                        BoundedString(GetOptionalString(entry, L"alias", L""), apc::limits::c_maxDeviceAliasCharacters);
                    const auto legacy = GetOptionalBoolean(entry, L"autoReconnect", false);
                    device.ConnectOnStartup = GetOptionalBoolean(entry, L"connectOnStartup", legacy);
                    device.ReconnectOnConnectionLoss = GetOptionalBoolean(entry, L"reconnectOnConnectionLoss", legacy);
                    loaded.Devices.push_back(std::move(device));
                } catch (...) {
                    DebugTrace(L"[SettingsStore] skipping invalid device entry");
                }
            }
        std::unordered_set<std::wstring> recentIds;
        if (auto ids = GetOptionalArray(json, L"lastConnectedIds"))
            for (auto value : ids) {
                if (loaded.LastConnectedIds.size() == apc::limits::c_maxPersistedDeviceCount) break;
                if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) continue;
                auto identifier = std::wstring(value.GetString());
                if (!identifier.empty() &&
                    apc::limits::IsBoundedUtf16(identifier, apc::limits::c_maxDeviceIdCharacters) &&
                    recentIds.insert(identifier).second)
                    loaded.LastConnectedIds.push_back(std::move(identifier));
            }
    } catch (std::exception const& exception) {
        DebugTrace(L"[SettingsStore] load failed: {0}", util::Utf8ToUtf16(exception.what()));
        m_impl->storage->PreserveCorrupt(path);
        m_impl->CompleteLoadWithoutCommit();
        return;
    } catch (...) {
        DebugTrace(L"[SettingsStore] load failed");
        m_impl->storage->PreserveCorrupt(path);
        m_impl->CompleteLoadWithoutCommit();
        return;
    }
    std::vector<Impl::SubscriptionStatePtr> subscriptionsToNotify;
    SettingsSnapshot snapshot;
    try {
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->shutdownRequested || m_impl->revision != 0) {
                m_impl->loadActive = false;
                m_impl->changed.notify_all();
                return;
            }

            std::unique_lock publicationLock(m_impl->publicationMutex, std::defer_lock);
            const auto shouldPublish = !m_impl->closing && !m_impl->subscriptions.empty();
            if (shouldPublish) publicationLock.lock();
            if (shouldPublish) {
                snapshot = {loaded, 1, false};
                subscriptionsToNotify.reserve(m_impl->subscriptions.size());
                for (auto const& [_, entry] : m_impl->subscriptions) {
                    subscriptionsToNotify.push_back(entry.State);
                }
                m_impl->EnqueuePublicationWithLockHeld(std::move(snapshot), std::move(subscriptionsToNotify));
            }

            m_impl->data = std::move(loaded);
            m_impl->revision = 1;
            m_impl->persistedRevision = 1;
            m_impl->loadActive = false;
        }
        m_impl->changed.notify_all();
    } catch (...) {
        m_impl->CompleteLoadWithoutCommit();
        throw;
    }
    m_impl->DrainPublications();
}

namespace {
DeviceSettings* FindDevice(SettingsData& data, std::wstring_view id) {
    const auto it = std::ranges::find(data.Devices, id, &DeviceSettings::Id);
    return it == data.Devices.end() ? nullptr : &*it;
}
} // namespace

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
RecordConnectedDeviceResult SettingsStore::RecordConnectedDevice(std::wstring_view deviceId,
                                                                 std::wstring_view deviceName) {
    RecordConnectedDeviceResult result;
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters) ||
        !apc::limits::IsBoundedUtf16(deviceName, apc::limits::c_maxDeviceNameCharacters)) {
        result.Mutation = {SettingsMutationStatus::Rejected, Snapshot().Revision};
        return result;
    }
    result.Mutation =
        m_impl->Commit([&result, deviceId = std::wstring(deviceId), deviceName = std::wstring(deviceName)](auto& data) {
            result.EffectiveReconnectOnConnectionLoss = data.GlobalReconnectOnConnectionLoss;
            auto* device = FindDevice(data, deviceId);
            if (!device) {
                if (data.Devices.size() < apc::limits::c_maxPersistedDeviceCount) {
                    data.Devices.push_back(
                        {deviceId, deviceName, L"", data.GlobalConnectOnStartup, data.GlobalReconnectOnConnectionLoss});
                    device = &data.Devices.back();
                    result.AddedDevice = true;
                    result.PresentationChanged = true;
                }
            } else if (!deviceName.empty() && device->Name != deviceName) {
                device->Name = deviceName;
                result.PresentationChanged = device->Alias.empty() && !data.PrivacyModeEnabled;
            }
            if (device) {
                result.ConnectOnStartup = device->ConnectOnStartup;
                result.EffectiveReconnectOnConnectionLoss =
                    result.EffectiveReconnectOnConnectionLoss || device->ReconnectOnConnectionLoss;
            }
            const auto before = data.LastConnectedIds;
            std::erase(data.LastConnectedIds, deviceId);
            data.LastConnectedIds.insert(data.LastConnectedIds.begin(), deviceId);
            if (data.LastConnectedIds.size() > apc::limits::c_maxPersistedDeviceCount) data.LastConnectedIds.pop_back();
            return result.AddedDevice || result.PresentationChanged || data.LastConnectedIds != before;
        });
    return result;
}
SettingsMutationResult SettingsStore::RecordUpdateCheckMetadata(std::int64_t unixSeconds,
                                                                std::optional<std::wstring> notifiedVersion) {
    if (notifiedVersion && !apc::limits::IsBoundedUtf16(*notifiedVersion, apc::limits::c_maxVersionCharacters))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([unixSeconds, notifiedVersion = std::move(notifiedVersion)](auto& data) {
        const auto changed = data.LastUpdateCheckUnixSeconds != unixSeconds ||
                             (notifiedVersion && data.LastNotifiedUpdateVersion != *notifiedVersion);
        data.LastUpdateCheckUnixSeconds = unixSeconds;
        if (notifiedVersion) data.LastNotifiedUpdateVersion = *notifiedVersion;
        return changed;
    });
}

bool SettingsStore::FlushNow(unsigned int maximumAttempts) noexcept {
    return m_impl->FlushSynchronously(maximumAttempts);
}

bool SettingsStore::Shutdown(SettingsShutdownMode mode, unsigned int maximumAttempts) noexcept {
    const auto impl = m_impl;
    if (!impl) return true;
    bool isShutdownExecutor = false;
    bool shutdownResult = false;
    {
        std::unique_lock lock(impl->mutex);
        if (impl->closing) {
            impl->shutdownChanged.wait(lock, [&] { return impl->shutdownCoreComplete; });
            shutdownResult = impl->shutdownResult;
        } else {
            isShutdownExecutor = true;
            impl->closing = true;
            impl->timerArmed = false;
            impl->DeactivateSubscriptionsLocked();
        }
    }

    if (!isShutdownExecutor) {
        impl->WaitForPublicationDrain();
        return shutdownResult;
    }

    impl->changed.notify_all();
    {
        std::unique_lock lock(impl->mutex);
        // Close admission first, then let an already-admitted read publish its state without callbacks. This
        // gives the final flush the loaded snapshot while rejecting post-shutdown mutations and subscriptions.
        impl->changed.wait(lock, [&] { return !impl->loadActive; });
    }
    shutdownResult = mode == SettingsShutdownMode::Flush ? impl->FlushSynchronously(maximumAttempts) : true;
    {
        std::scoped_lock lock(impl->mutex);
        impl->shutdownRequested = true;
        impl->discardRequested = mode == SettingsShutdownMode::DiscardStartupFailure;
        impl->timerArmed = false;
    }
    {
        std::scoped_lock publicationLock(impl->publicationMutex);
        impl->pendingPublications.clear();
    }
    impl->changed.notify_all();
    if (impl->worker.joinable()) {
        impl->worker.request_stop();
        impl->worker.join();
    }
    {
        std::scoped_lock lock(impl->mutex);
        impl->shutdownResult = shutdownResult;
        impl->shutdownCoreComplete = true;
    }
    impl->shutdownChanged.notify_all();
    impl->WaitForPublicationDrain();
    return shutdownResult;
}
