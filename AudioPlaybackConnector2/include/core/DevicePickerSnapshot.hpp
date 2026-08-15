#pragma once

#include <core/DeviceInventoryGeneration.hpp>
#include <core/DevicePickerTypes.hpp>

#include <chrono>
#include <span>
#include <string_view>
#include <vector>

namespace apc::device_picker {

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
