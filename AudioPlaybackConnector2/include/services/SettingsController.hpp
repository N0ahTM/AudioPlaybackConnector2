#pragma once

#include <core/Settings.hpp>

#include <functional>

class DeviceManager;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Settings Controller ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ISettingsController {
public:
    enum class PresentationChangeKind { Content, Language, Appearance };
    using PresentationChangedCallback = std::function<void(PresentationChangeKind)>;

    virtual ~ISettingsController() = default;

    virtual SettingsData Snapshot() const = 0;
    virtual void SetPresentationChangedCallback(PresentationChangedCallback callback) = 0;
    virtual void SetGlobalConnectOnStartup(bool enabled) = 0;
    virtual void SetGlobalReconnectOnConnectionLoss(bool enabled) = 0;
    virtual void SetAllowIncomingConnections(bool enabled) = 0;
    virtual void SetStartWithWindows(bool enabled) = 0;
    virtual void SetShowNotifications(bool enabled) = 0;
    virtual void SetSystemBackdropEffects(bool enabled) = 0;
    virtual void SetLanguage(std::wstring language) = 0;
    virtual void SetPrivacyMode(bool enabled) = 0;
    virtual bool SetSettingsWindowBounds(PersistedWindowBounds bounds) = 0;
    virtual bool ClearSettingsWindowBounds() = 0;
    virtual void Save() = 0;
    virtual void SetDeviceConnectOnStartup(std::wstring const& deviceId, bool enabled) = 0;
    virtual void SetDeviceReconnectOnConnectionLoss(std::wstring const& deviceId, bool enabled) = 0;
    virtual bool
    SetDeviceAlias(std::wstring const& deviceId, std::wstring alias, std::wstring const& deviceName = {}) = 0;
    virtual void SetDefaultDeviceId(std::wstring const& deviceId) = 0;
    virtual void ClearDefaultDevice() = 0;
    [[nodiscard]] virtual std::size_t ConnectedDeviceCount() const = 0;
    virtual void ForgetDevice(std::wstring const& deviceId) = 0;
};

class SettingsController final : public ISettingsController {
public:
    SettingsController(std::shared_ptr<Settings> settings,
                       std::weak_ptr<DeviceManager> deviceManager,
                       std::function<void()> requestSave = {});

    SettingsData Snapshot() const override;
    void SetPresentationChangedCallback(PresentationChangedCallback callback) override;
    void SetGlobalConnectOnStartup(bool enabled) override;
    void SetGlobalReconnectOnConnectionLoss(bool enabled) override;
    void SetAllowIncomingConnections(bool enabled) override;
    void SetStartWithWindows(bool enabled) override;
    void SetShowNotifications(bool enabled) override;
    void SetSystemBackdropEffects(bool enabled) override;
    void SetLanguage(std::wstring language) override;
    void SetPrivacyMode(bool enabled) override;
    bool SetSettingsWindowBounds(PersistedWindowBounds bounds) override;
    bool ClearSettingsWindowBounds() override;
    void Save() override;
    void SetDeviceConnectOnStartup(std::wstring const& deviceId, bool enabled) override;
    void SetDeviceReconnectOnConnectionLoss(std::wstring const& deviceId, bool enabled) override;
    bool SetDeviceAlias(std::wstring const& deviceId, std::wstring alias, std::wstring const& deviceName = {}) override;
    void SetDefaultDeviceId(std::wstring const& deviceId) override;
    void ClearDefaultDevice() override;
    [[nodiscard]] std::size_t ConnectedDeviceCount() const override;
    void ForgetDevice(std::wstring const& deviceId) override;

private:
    void RequestSave();
    void NotifyPresentationChanged(PresentationChangeKind kind = PresentationChangeKind::Content);

    std::shared_ptr<Settings> m_settings;
    std::weak_ptr<DeviceManager> m_deviceManager;
    std::function<void()> m_requestSave;
    PresentationChangedCallback m_presentationChanged;
};
