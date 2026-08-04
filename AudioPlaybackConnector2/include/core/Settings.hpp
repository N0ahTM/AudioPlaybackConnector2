#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <filesystem>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Data Structures ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct DeviceSettings {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    bool ConnectOnStartup = false;
    bool ReconnectOnConnectionLoss = false;

    bool operator==(DeviceSettings const&) const = default;
};

enum class DefaultDeviceMode { LastConnected, SpecificDevice };

struct PersistedWindowBounds {
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Width = 0;
    int32_t Height = 0;
    uint32_t Dpi = USER_DEFAULT_SCREEN_DPI;

    bool operator==(PersistedWindowBounds const&) const = default;
};

struct SettingsData {
    bool GlobalConnectOnStartup = false;
    bool GlobalReconnectOnConnectionLoss = false;
    bool AllowIncomingConnections = false;
    bool StartWithWindows = false;
    bool ShowNotifications = true;
    std::wstring Language = L"system";
    int64_t LastUpdateCheckUnixSeconds = 0;
    std::wstring LastNotifiedUpdateVersion;
    std::optional<PersistedWindowBounds> SettingsWindowBounds;
    bool PrivacyModeEnabled = false;
    DefaultDeviceMode DefaultDevice = DefaultDeviceMode::LastConnected;
    std::wstring DefaultDeviceId;
    std::vector<DeviceSettings> Devices;
    std::vector<std::wstring> LastConnectedIds;

    bool operator==(SettingsData const&) const = default;
};

template <typename Guard> class LockedMutableSettingsDataReference {
public:
    LockedMutableSettingsDataReference(Guard guard, SettingsData& data, std::atomic<std::uint64_t>& revision)
        : m_guard(std::move(guard)), m_data(data), m_revision(&revision) {}

    LockedMutableSettingsDataReference(LockedMutableSettingsDataReference const&) = delete;
    LockedMutableSettingsDataReference& operator=(LockedMutableSettingsDataReference const&) = delete;
    LockedMutableSettingsDataReference(LockedMutableSettingsDataReference&& other) noexcept
        : m_guard(std::move(other.m_guard)), m_data(other.m_data), m_revision(std::exchange(other.m_revision, nullptr)),
          m_changed(std::exchange(other.m_changed, false)) {}
    LockedMutableSettingsDataReference& operator=(LockedMutableSettingsDataReference&&) = delete;

    ~LockedMutableSettingsDataReference() {
        if (m_revision && m_changed) m_revision->fetch_add(1, std::memory_order_release);
    }

    void MarkChanged() noexcept { m_changed = true; }
    SettingsData* operator->() { return &m_data; }
    SettingsData& operator*() { return m_data; }
    SettingsData& Get() { return m_data; }

private:
    Guard m_guard;
    SettingsData& m_data;
    std::atomic<std::uint64_t>* m_revision;
    bool m_changed = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings //////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

template <typename T, typename Guard> class LockedSettingsDataReference {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    LockedSettingsDataReference(Guard guard, T& data) : m_guard(std::move(guard)), m_data(data) {}

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    T* operator->() { return &m_data; }
    const T* operator->() const { return &m_data; }
    T& operator*() { return m_data; }
    const T& operator*() const { return m_data; }
    T& Get() { return m_data; }
    const T& Get() const { return m_data; }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    Guard m_guard;
    T& m_data;
};

class Settings {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Load(HINSTANCE hInst);
    bool Save(HINSTANCE hInst);
    [[nodiscard]] bool HasUnsavedChanges() const noexcept {
        return m_revision.load(std::memory_order_acquire) != m_savedRevision.load(std::memory_order_acquire);
    }

    auto LockShared() const { return m_lock.lock_shared(); }
    auto LockExclusive() { return m_lock.lock_exclusive(); }

    auto LockSharedData() const {
        return LockedSettingsDataReference<const SettingsData, decltype(m_lock.lock_shared())>(m_lock.lock_shared(),
                                                                                               m_data);
    }
    auto LockExclusiveData() {
        auto guard = m_lock.lock_exclusive();
        return LockedMutableSettingsDataReference<decltype(guard)>(std::move(guard), m_data, m_revision);
    }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::filesystem::path GetPath(HINSTANCE hInst) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    SettingsData m_data;
    mutable wil::srwlock m_lock;
    std::mutex m_persistenceMutex;
    std::atomic<std::uint64_t> m_revision{0};
    std::atomic<std::uint64_t> m_savedRevision{0};
    static constexpr auto c_fileName = L"AudioPlaybackConnector2.json";
};
