#pragma once

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Data Structures ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct DeviceSettings {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    bool AutoReconnect = false;
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
    bool GlobalAutoReconnect = false;
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
    void Save(HINSTANCE hInst);

    auto LockShared() const { return m_lock.lock_shared(); }
    auto LockExclusive() { return m_lock.lock_exclusive(); }

    auto LockSharedData() const {
        return LockedSettingsDataReference<const SettingsData, decltype(m_lock.lock_shared())>(m_lock.lock_shared(),
                                                                                               m_data);
    }
    auto LockExclusiveData() {
        return LockedSettingsDataReference<SettingsData, decltype(m_lock.lock_exclusive())>(m_lock.lock_exclusive(),
                                                                                            m_data);
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
    static constexpr auto c_fileName = L"AudioPlaybackConnector2.json";
};
