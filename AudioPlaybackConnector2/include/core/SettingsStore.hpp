#pragma once

#include <core/SettingsData.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct SettingsSnapshot {
    SettingsData Data;
    std::uint64_t Revision = 0;
    bool IsDirty = false;

    bool operator==(SettingsSnapshot const&) const = default;
};

enum class SettingsMutationStatus { Rejected, Unchanged, Applied };

struct SettingsMutationResult {
    SettingsMutationStatus Status = SettingsMutationStatus::Unchanged;
    std::uint64_t Revision = 0;

    [[nodiscard]] bool IsApplied() const noexcept { return Status == SettingsMutationStatus::Applied; }
};

struct RecordConnectedDeviceResult {
    SettingsMutationResult Mutation;
    bool AddedDevice = false;
    bool PresentationChanged = false;
    bool ConnectOnStartup = false;
    bool EffectiveReconnectOnConnectionLoss = false;
};

enum class SettingsShutdownMode { Flush, DiscardStartupFailure };

// The only persistence boundary. Production uses complete file writes and atomic replacement;
// deterministic tests can block or fail this boundary without timing the store worker.
class SettingsStoreStorage {
public:
    virtual ~SettingsStoreStorage() = default;
    [[nodiscard]] virtual std::optional<std::string> Read(std::filesystem::path const& path) = 0;
    [[nodiscard]] virtual bool WriteAtomically(std::filesystem::path const& path, std::string_view bytes) = 0;
    virtual void PreserveCorrupt(std::filesystem::path const& path) noexcept = 0;
};

// Owns all mutable settings and its sole persistence worker. Public operations are thread-safe;
// notifications run synchronously on the caller that committed the change, outside store locks.
class SettingsStore final {
public:
    class Subscription final {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        void Reset() noexcept;

    private:
        friend class SettingsStore;
        explicit Subscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}

        std::function<void()> m_unsubscribe;
    };

    using SnapshotCallback = std::function<void(SettingsSnapshot const&)>;

    explicit SettingsStore(std::filesystem::path persistenceDirectory = {},
                           std::shared_ptr<SettingsStoreStorage> storage = {});
    ~SettingsStore();
    SettingsStore(SettingsStore const&) = delete;
    SettingsStore& operator=(SettingsStore const&) = delete;

    void Load();
    [[nodiscard]] SettingsSnapshot Snapshot() const;
    [[nodiscard]] Subscription Subscribe(SnapshotCallback callback);

    [[nodiscard]] SettingsMutationResult SetGlobalConnectOnStartup(bool enabled);
    [[nodiscard]] SettingsMutationResult SetGlobalReconnectOnConnectionLoss(bool enabled);
    [[nodiscard]] SettingsMutationResult SetAllowIncomingConnections(bool enabled);
    [[nodiscard]] SettingsMutationResult SetStartWithWindows(bool enabled);
    [[nodiscard]] SettingsMutationResult SetShowNotifications(bool enabled);
    [[nodiscard]] SettingsMutationResult SetUseSystemBackdropEffects(bool enabled);
    [[nodiscard]] SettingsMutationResult SetLanguage(std::wstring_view language);
    [[nodiscard]] SettingsMutationResult SetPrivacyModeEnabled(bool enabled);
    [[nodiscard]] SettingsMutationResult SetSettingsWindowBounds(std::optional<PersistedWindowBounds> bounds);
    [[nodiscard]] SettingsMutationResult SetDeviceConnectOnStartup(std::wstring_view deviceId, bool enabled);
    [[nodiscard]] SettingsMutationResult SetDeviceReconnectOnConnectionLoss(std::wstring_view deviceId, bool enabled);
    [[nodiscard]] SettingsMutationResult SetDeviceAlias(std::wstring_view deviceId,
                                                        std::wstring_view alias,
                                                        std::optional<std::wstring> deviceName = std::nullopt);
    [[nodiscard]] SettingsMutationResult SetDefaultDevice(std::wstring_view deviceId);
    [[nodiscard]] SettingsMutationResult ClearDefaultDevice();
    [[nodiscard]] SettingsMutationResult ForgetDevice(std::wstring_view deviceId);
    [[nodiscard]] RecordConnectedDeviceResult RecordConnectedDevice(std::wstring_view deviceId,
                                                                    std::wstring_view deviceName);
    [[nodiscard]] SettingsMutationResult
    RecordUpdateCheckMetadata(std::int64_t unixSeconds, std::optional<std::wstring> notifiedVersion = std::nullopt);

    // A synchronous boundary for suspend and normal shutdown. It waits for the active attempt,
    // then writes the newest captured revision without ever overlapping the background worker.
    [[nodiscard]] bool FlushNow(unsigned int maximumAttempts = 1) noexcept;
    [[nodiscard]] bool Shutdown(SettingsShutdownMode mode, unsigned int maximumAttempts = 3) noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};
