#include <core/DeviceInventoryGeneration.hpp>

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

} // namespace apc::device_picker
