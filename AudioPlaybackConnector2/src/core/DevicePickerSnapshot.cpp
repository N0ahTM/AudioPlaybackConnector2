#include <algorithm>
#include <unordered_map>
#include <utility>

#include <core/DevicePickerSnapshot.hpp>

namespace apc::device_picker {

DeviceInventoryGeneration::Value DeviceInventoryGeneration::Capture() const noexcept {
    return m_value.load(std::memory_order_acquire);
}

void DeviceInventoryGeneration::Invalidate() noexcept {
    static_cast<void>(m_value.fetch_add(1, std::memory_order_acq_rel));
}

bool DeviceInventoryGeneration::TryInvalidate() noexcept {
    if (!m_active.load(std::memory_order_acquire)) return false;
    Invalidate();
    return m_active.load(std::memory_order_acquire);
}

void DeviceInventoryGeneration::Deactivate() noexcept {
    m_active.store(false, std::memory_order_release);
}

bool DeviceInventoryGeneration::ChangedSince(Value captured) const noexcept {
    return Capture() != captured;
}

DevicePickerSnapshotCache::DevicePickerSnapshotCache(std::chrono::milliseconds freshnessLifetime)
    : m_freshnessLifetime(std::max(freshnessLifetime, std::chrono::milliseconds::zero())) {}

void DevicePickerSnapshotCache::ReplaceInventory(std::vector<DeviceIdentity> devices, TimePoint refreshedAt) {
    std::unordered_set<std::wstring> seenIds;
    std::vector<DeviceIdentity> normalized;
    normalized.reserve(devices.size());
    for (auto& device : devices) {
        if (device.Id.empty() || !seenIds.insert(device.Id).second) continue;
        if (device.Name.empty()) device.Name = device.Id;
        normalized.push_back(std::move(device));
    }

    m_inventory = std::move(normalized);
    m_inventoryRefreshedAt = refreshedAt;
    m_hasInventory = true;
    m_inventoryInvalidated = false;
    AdvanceGeneration(m_inventoryGeneration);
}

void DevicePickerSnapshotCache::InvalidateInventory() noexcept {
    m_inventoryInvalidated = true;
    AdvanceGeneration(m_inventoryGeneration);
    m_snapshot.InventoryFresh = false;
    m_snapshot.InventoryGeneration = m_inventoryGeneration;
}

void DevicePickerSnapshotCache::Clear() noexcept {
    auto snapshotGeneration = m_snapshot.Generation;
    AdvanceGeneration(snapshotGeneration);
    m_inventory.clear();
    m_snapshot = {};
    m_snapshot.Generation = snapshotGeneration;
    m_inventoryRefreshedAt = {};
    m_hasInventory = false;
    m_inventoryInvalidated = true;
    AdvanceGeneration(m_inventoryGeneration);
    m_snapshot.InventoryGeneration = m_inventoryGeneration;
}

bool DevicePickerSnapshotCache::HasInventory() const noexcept {
    return m_hasInventory;
}

bool DevicePickerSnapshotCache::IsInventoryFresh(TimePoint now) const noexcept {
    if (!m_hasInventory || m_inventoryInvalidated || now < m_inventoryRefreshedAt) return false;
    return now - m_inventoryRefreshedAt <= m_freshnessLifetime;
}

DevicePickerSnapshot const& DevicePickerSnapshotCache::Refresh(DeviceActivitySnapshot const& activity,
                                                               std::span<DevicePresentationSetting const> settings,
                                                               bool privacyModeEnabled,
                                                               std::wstring_view privacyDisplayName,
                                                               TimePoint now) {
    std::unordered_map<std::wstring_view, DevicePresentationSetting const*> settingsById;
    settingsById.reserve(settings.size());
    for (auto const& setting : settings) {
        if (!setting.Id.empty()) settingsById.try_emplace(setting.Id, &setting);
    }

    DevicePickerSnapshot next;
    next.Generation = m_snapshot.Generation;
    AdvanceGeneration(next.Generation);
    next.InventoryGeneration = m_inventoryGeneration;
    next.ConnectedDeviceCount = activity.ConnectedIds.size();
    next.InventoryFresh = IsInventoryFresh(now);
    next.PrivacyModeEnabled = privacyModeEnabled;
    next.Items.reserve(m_inventory.size());

    for (auto const& device : m_inventory) {
        DeviceSnapshotItem item;
        item.Id = device.Id;
        item.Name = device.Name.empty() ? device.Id : device.Name;

        if (auto setting = settingsById.find(device.Id); setting != settingsById.end()) {
            if (!setting->second->Name.empty()) item.Name = setting->second->Name;
            item.Alias = setting->second->Alias;
        }

        if (!item.Alias.empty()) {
            item.DisplayName = item.Alias;
        } else if (privacyModeEnabled) {
            item.DisplayName = privacyDisplayName;
        } else {
            item.DisplayName = item.Name;
        }
        if (item.DisplayName.empty()) item.DisplayName = item.Id;

        item.IsConnected = activity.ConnectedIds.contains(item.Id);
        item.IsBusy = activity.BusyIds.contains(item.Id);
        next.Items.push_back(std::move(item));
    }

    m_snapshot = std::move(next);
    return m_snapshot;
}

DevicePickerSnapshot const& DevicePickerSnapshotCache::CachedSnapshot() const noexcept {
    return m_snapshot;
}

bool DevicePickerSnapshotCache::CanSelect(std::wstring_view id) const noexcept {
    if (id.empty()) return false;
    auto const item = std::ranges::find(m_snapshot.Items, id, &DeviceSnapshotItem::Id);
    return item != m_snapshot.Items.end() && !item->IsConnected && !item->IsBusy;
}

void DevicePickerSnapshotCache::AdvanceGeneration(std::uint64_t& generation) noexcept {
    ++generation;
    if (generation == 0) ++generation;
}

} // namespace apc::device_picker
