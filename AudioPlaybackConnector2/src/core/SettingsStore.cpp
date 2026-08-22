#include <pch.h>

#include <core/SettingsStore.hpp>

#include <core/SettingsLimits.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>
#include <util/Util.hpp>

#include <condition_variable>
#include <deque>
#include <atomic>
#include <thread>
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
        const auto cleanup = wil::scope_exit([&] { DeleteFileW(temporaryPath.c_str()); });
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
    explicit Impl(std::filesystem::path directory, std::shared_ptr<SettingsStoreStorage> persistenceStorage)
        : persistenceDirectory(std::move(directory)), storage(std::move(persistenceStorage)) {
        if (!storage) storage = std::make_shared<FilesystemSettingsStoreStorage>();
    }

    std::mutex mutex;
    std::mutex publicationMutex;
    std::condition_variable publicationChanged;
    std::condition_variable changed;
    SettingsData data;
    std::uint64_t revision = 0;
    std::uint64_t persistedRevision = 0;
    std::uint64_t nextSubscriptionId = 0;
    struct SubscriptionEntry {
        SnapshotCallback Callback;
        std::shared_ptr<std::atomic_bool> IsActive;
    };
    std::unordered_map<std::uint64_t, SubscriptionEntry> subscriptions;
    struct Publication {
        SettingsSnapshot Snapshot;
        std::vector<SnapshotCallback> Callbacks;
    };
    std::deque<Publication> pendingPublications;
    bool publishing = false;
    bool callbackActive = false;
    std::filesystem::path persistenceDirectory;
    std::shared_ptr<SettingsStoreStorage> storage;
    bool timerArmed = false;
    std::chrono::steady_clock::time_point due{};
    bool writerActive = false;
    bool closing = false;
    bool shutdownRequested = false;
    bool discardRequested = false;
    bool synchronousFallback = false;
    bool workerStartKnown = false;
    bool loadClaimed = false;
    unsigned int failures = 0;
    std::jthread worker;

    [[nodiscard]] SettingsSnapshot SnapshotLocked() const { return {data, revision, revision != persistedRevision}; }

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
                changed.wait(lock, [&] { return !writerActive; });
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
            changed.wait(lock, [&] { return stopToken.stop_requested() || shutdownRequested || timerArmed; });
            if (stopToken.stop_requested() || shutdownRequested) return;
            const auto scheduled = due;
            if (changed.wait_until(lock, scheduled, [&] {
                    return stopToken.stop_requested() || shutdownRequested || due != scheduled;
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

    void EnqueuePublicationLocked(SettingsSnapshot snapshot, std::vector<SnapshotCallback> callbacks) {
        std::scoped_lock publicationLock(publicationMutex);
        pendingPublications.push_back({std::move(snapshot), std::move(callbacks)});
    }

    void DrainPublications() noexcept {
        {
            std::scoped_lock lock(publicationMutex);
            if (publishing) return;
            publishing = true;
        }
        while (true) {
            Publication publication;
            {
                std::scoped_lock lock(publicationMutex);
                if (pendingPublications.empty()) {
                    publishing = false;
                    publicationChanged.notify_all();
                    return;
                }
                publication = std::move(pendingPublications.front());
                pendingPublications.pop_front();
            }
            for (auto const& callback : publication.Callbacks) {
                {
                    std::scoped_lock stateLock(mutex);
                    if (closing) break;
                }
                {
                    std::scoped_lock publicationLock(publicationMutex);
                    callbackActive = true;
                }
                try {
                    callback(publication.Snapshot);
                } catch (...) {
                    DebugTrace(L"[SettingsStore] subscriber threw");
                }
                {
                    std::scoped_lock publicationLock(publicationMutex);
                    callbackActive = false;
                }
                publicationChanged.notify_all();
            }
        }
    }

    template <typename Mutation> [[nodiscard]] SettingsMutationResult Commit(Mutation&& mutation) {
        std::vector<SnapshotCallback> callbacks;
        SettingsSnapshot snapshot;
        std::uint64_t committedRevision = 0;
        bool useSynchronousFallback = false;
        {
            std::scoped_lock lock(mutex);
            if (closing || shutdownRequested) return {SettingsMutationStatus::Rejected, revision};
            if (!mutation(data)) return {SettingsMutationStatus::Unchanged, revision};
            ++revision;
            committedRevision = revision;
            timerArmed = true;
            due = std::chrono::steady_clock::now() + c_debounceDelay;
            snapshot = SnapshotLocked();
            for (auto const& [_, entry] : subscriptions) {
                callbacks.push_back([entry](SettingsSnapshot const& published) {
                    if (entry.IsActive->load(std::memory_order_acquire)) entry.Callback(published);
                });
            }
            EnqueuePublicationLocked(std::move(snapshot), std::move(callbacks));
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
    auto active = std::make_shared<std::atomic_bool>(true);
    m_impl->subscriptions.emplace(identifier, Impl::SubscriptionEntry{std::move(callback), active});
    std::weak_ptr weak = m_impl;
    return Subscription([weak, active, identifier] {
        active->store(false, std::memory_order_release);
        if (auto impl = weak.lock()) {
            std::scoped_lock lock(impl->mutex);
            impl->subscriptions.erase(identifier);
        }
    });
}

void SettingsStore::Load() {
    SettingsData loaded;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->loadClaimed || m_impl->closing || m_impl->shutdownRequested) return;
        m_impl->loadClaimed = true;
    }
    const auto path = m_impl->Path();
    try {
        const auto bytes = m_impl->storage->Read(path);
        if (!bytes || bytes->empty()) return;
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
            PersistedWindowBounds bounds{
                GetOptionalInt32(boundsJson, L"x", 0),
                GetOptionalInt32(boundsJson, L"y", 0),
                GetOptionalInt32(boundsJson, L"width", 0),
                GetOptionalInt32(boundsJson, L"height", 0),
                static_cast<std::uint32_t>(GetOptionalInt32(boundsJson, L"dpi", USER_DEFAULT_SCREEN_DPI))};
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
        return;
    } catch (...) {
        DebugTrace(L"[SettingsStore] load failed");
        m_impl->storage->PreserveCorrupt(path);
        return;
    }
    std::vector<SnapshotCallback> callbacks;
    SettingsSnapshot snapshot;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->shutdownRequested || m_impl->closing || m_impl->revision != 0) return;
        m_impl->data = std::move(loaded);
        ++m_impl->revision;
        m_impl->persistedRevision = m_impl->revision;
        snapshot = m_impl->SnapshotLocked();
        for (auto const& [_, entry] : m_impl->subscriptions) {
            callbacks.push_back([entry](SettingsSnapshot const& published) {
                if (entry.IsActive->load(std::memory_order_acquire)) entry.Callback(published);
            });
        }
        m_impl->EnqueuePublicationLocked(std::move(snapshot), std::move(callbacks));
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
    return m_impl->Commit([=](auto& data) {
        auto* device = FindDevice(data, deviceId);
        if (!device) return false;
        return std::exchange(device->ConnectOnStartup, enabled) != enabled;
    });
}
SettingsMutationResult SettingsStore::SetDeviceReconnectOnConnectionLoss(std::wstring_view deviceId, bool enabled) {
    return m_impl->Commit([=](auto& data) {
        auto* device = FindDevice(data, deviceId);
        if (!device) return false;
        return std::exchange(device->ReconnectOnConnectionLoss, enabled) != enabled;
    });
}
SettingsMutationResult SettingsStore::SetDeviceAlias(std::wstring_view deviceId,
                                                     std::wstring_view alias,
                                                     std::optional<std::wstring> deviceName) {
    if (deviceId.empty() || !apc::limits::IsBoundedUtf16(deviceId, apc::limits::c_maxDeviceIdCharacters) ||
        !apc::limits::IsBoundedUtf16(alias, apc::limits::c_maxDeviceAliasCharacters) ||
        (deviceName && !apc::limits::IsBoundedUtf16(*deviceName, apc::limits::c_maxDeviceNameCharacters)))
        return {SettingsMutationStatus::Rejected, Snapshot().Revision};
    return m_impl->Commit([deviceId = std::wstring(deviceId),
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
            return true;
        }
        const auto shouldUpdateName = deviceName && !deviceName->empty() && device->Name != *deviceName;
        const auto changed = device->Alias != alias || shouldUpdateName;
        device->Alias = alias;
        if (shouldUpdateName) device->Name = *deviceName;
        return changed;
    });
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
                    data.GlobalReconnectOnConnectionLoss || device->ReconnectOnConnectionLoss;
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
    if (!m_impl) return true;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->shutdownRequested || m_impl->closing) return true;
        m_impl->closing = true;
        m_impl->timerArmed = false;
    }
    m_impl->changed.notify_all();
    const auto flushed = mode == SettingsShutdownMode::Flush ? FlushNow(maximumAttempts) : true;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->shutdownRequested) return flushed;
        m_impl->shutdownRequested = true;
        m_impl->discardRequested = mode == SettingsShutdownMode::DiscardStartupFailure;
        m_impl->timerArmed = false;
        m_impl->subscriptions.clear();
    }
    {
        std::scoped_lock publicationLock(m_impl->publicationMutex);
        m_impl->pendingPublications.clear();
    }
    m_impl->changed.notify_all();
    if (m_impl->worker.joinable()) {
        m_impl->worker.request_stop();
        m_impl->worker.join();
    }
    std::unique_lock publicationLock(m_impl->publicationMutex);
    m_impl->publicationChanged.wait(publicationLock, [&] { return !m_impl->publishing && !m_impl->callbackActive; });
    return flushed;
}
