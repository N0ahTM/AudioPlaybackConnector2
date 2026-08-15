#pragma once

#include <atomic>
#include <cstdint>

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

} // namespace apc::device_picker
