#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace apc::device_picker {

class DeviceInventoryGeneration {
public:
    using Value = std::uint64_t;

    [[nodiscard]] Value Capture() const noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool TryInvalidate() noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] bool ChangedSince(Value captured) const noexcept;

private:
    std::atomic<Value> m_value = 0;
    std::atomic<bool> m_active = true;
};

struct DeviceIdentity {
    std::wstring Id;
    std::wstring Name;
};

struct DeviceInventorySnapshot {
    std::uint64_t Generation = 0;
    bool EnumerationComplete = false;
    std::vector<DeviceIdentity> Devices;
};

struct DevicePresentationSetting {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
};

struct DeviceActivitySnapshot {
    std::unordered_set<std::wstring> ConnectedIds;
    std::unordered_set<std::wstring> BusyIds;
};

struct DeviceSnapshotItem {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    bool IsConnected = false;
    bool IsBusy = false;

    bool operator==(DeviceSnapshotItem const&) const = default;
};

struct DevicePickerSnapshot {
    std::uint64_t Generation = 0;
    std::uint64_t InventoryGeneration = 0;
    std::size_t ConnectedDeviceCount = 0;
    bool InventoryFresh = false;
    bool PrivacyModeEnabled = false;
    std::vector<DeviceSnapshotItem> Items;
};

class DevicePickerSnapshotCache {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit DevicePickerSnapshotCache(std::chrono::milliseconds freshnessLifetime = std::chrono::seconds(10));

    void ReplaceInventory(std::vector<DeviceIdentity> devices, TimePoint refreshedAt = Clock::now());
    void InvalidateInventory() noexcept;
    void Clear() noexcept;

    [[nodiscard]] bool HasInventory() const noexcept;
    [[nodiscard]] bool IsInventoryFresh(TimePoint now = Clock::now()) const noexcept;
    [[nodiscard]] DevicePickerSnapshot const& Refresh(DeviceActivitySnapshot const& activity,
                                                      std::span<DevicePresentationSetting const> settings,
                                                      bool privacyModeEnabled,
                                                      std::wstring_view privacyDisplayName,
                                                      TimePoint now = Clock::now());
    [[nodiscard]] DevicePickerSnapshot const& CachedSnapshot() const noexcept;
    [[nodiscard]] bool CanSelect(std::wstring_view id) const noexcept;

private:
    static void AdvanceGeneration(std::uint64_t& generation) noexcept;

    std::vector<DeviceIdentity> m_inventory;
    DevicePickerSnapshot m_snapshot;
    TimePoint m_inventoryRefreshedAt{};
    std::chrono::milliseconds m_freshnessLifetime;
    std::uint64_t m_inventoryGeneration = 0;
    bool m_hasInventory = false;
    bool m_inventoryInvalidated = true;
};

} // namespace apc::device_picker
