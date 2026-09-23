#pragma once

#include <core/SettingsData.hpp>
#include <core/SettingsStoreWakeup.hpp>
#include <util/Logger.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Types /////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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

struct DeviceAliasResult {
    SettingsMutationResult Mutation;
    bool DeviceExists = false;
};

enum class SettingsShutdownMode { Flush, DiscardStartupFailure };

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Storage Boundary //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// The only persistence boundary. Production uses complete file writes and atomic replacement;
// deterministic tests can block or fail this boundary without timing the store worker.
class SettingsStoreStorage {
public:
    virtual ~SettingsStoreStorage() = default;
    [[nodiscard]] virtual std::optional<std::string> Read(std::filesystem::path const& path) = 0;
    [[nodiscard]] virtual bool WriteAtomically(std::filesystem::path const& path, std::string_view bytes) = 0;
    // True only when the original bytes have been preserved away from the writable path.
    [[nodiscard]] virtual bool PreserveCorrupt(std::filesystem::path const& path) noexcept = 0;
};

// Owns all mutable settings and its sole persistence worker. Public operations are thread-safe.
// Subscriber callbacks are dispatched by a no-lock publication drain after commit; they may run on the
// committing thread or an already-active publisher. Reentrant changes are queued and drained in revision order.
class SettingsStore final {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Subscription //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    class Subscription final {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        // After this returns, another thread cannot enter this subscription's callback and any callback
        // already running on another thread has completed. A callback may reset itself; that case disables
        // future delivery but cannot wait for its own stack frame to return.
        void Reset() noexcept;

    private:
        friend class SettingsStore;
        explicit Subscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}

        std::function<void()> m_unsubscribe;
    };

    using SnapshotCallback = std::function<void(SettingsSnapshot const&)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit SettingsStore(std::filesystem::path persistenceDirectory = {},
                           std::shared_ptr<SettingsStoreStorage> storage = {},
                           std::shared_ptr<SettingsStoreWakeup> wakeup = {},
                           util::LogSink log = {});
    ~SettingsStore();
    SettingsStore(SettingsStore const&) = delete;
    SettingsStore& operator=(SettingsStore const&) = delete;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

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
    [[nodiscard]] SettingsMutationResult RememberDevice(std::wstring_view deviceId, std::wstring_view deviceName);
    [[nodiscard]] SettingsMutationResult SetDeviceReconnectOnConnectionLoss(std::wstring_view deviceId, bool enabled);
    [[nodiscard]] DeviceAliasResult SetDeviceAlias(std::wstring_view deviceId,
                                                   std::wstring_view alias,
                                                   std::optional<std::wstring> deviceName = std::nullopt);
    [[nodiscard]] SettingsMutationResult SetDefaultDevice(std::wstring_view deviceId);
    [[nodiscard]] SettingsMutationResult ClearDefaultDevice();
    [[nodiscard]] SettingsMutationResult ForgetDevice(std::wstring_view deviceId);
    [[nodiscard]] SettingsMutationResult
    RecordConnectedDevice(std::wstring_view deviceId, std::wstring_view deviceName, std::wstring usageDay = {});
    // Rating-prompt usage state; dates are local ISO YYYY-MM-DD days.
    [[nodiscard]] SettingsMutationResult RecordRatingPromptFirstLaunch(std::wstring today);
    [[nodiscard]] SettingsMutationResult MarkRatingPromptAsked();
    // A synchronous boundary for suspend and normal shutdown. It waits for the active attempt,
    // then writes the newest captured revision without ever overlapping the background worker.
    // Shutdown has one executor; concurrent callers receive its stored core result and wait for publication
    // drain unless they are the active publisher invoking shutdown from a callback.
    [[nodiscard]] bool FlushNow(unsigned int maximumAttempts = 1) noexcept;
    // One total budget covers I/O, worker completion, concurrent shutdown and callback drain. False means
    // incomplete persistence/drain; admission is nevertheless closed. Already-entered storage/callback code
    // retains its state until it returns. No later completion may change the public settings snapshot.
    [[nodiscard]] bool Shutdown(SettingsShutdownMode mode,
                                unsigned int maximumAttempts = 3,
                                std::chrono::milliseconds timeBudget = std::chrono::seconds(2)) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct Impl;
    std::shared_ptr<Impl> m_impl;
};
