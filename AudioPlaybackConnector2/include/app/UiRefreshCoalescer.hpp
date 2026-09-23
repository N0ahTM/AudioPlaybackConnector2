#pragma once

#include <sal.h>
#include <concurrencysal.h>
#include <cstdint>
#include <mutex>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UI Refresh Coalescer //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class UiRefreshCoalescer {
public:
    using Flags = std::uint32_t;

    [[nodiscard]] bool Request(Flags flags) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled || flags == 0) return false;
        m_pendingFlags |= flags;
        if (m_drainScheduled) {
            return false;
        }
        m_drainScheduled = true;
        return true;
    }

    [[nodiscard]] Flags BeginDrain() noexcept {
        std::scoped_lock lock(m_mutex);
        auto flags = m_pendingFlags;
        m_pendingFlags = 0;
        return flags;
    }

    [[nodiscard]] bool CompleteDrain() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled) return false;
        if (m_pendingFlags != 0) {
            return true;
        }
        m_drainScheduled = false;
        return false;
    }

    void Cancel() noexcept {
        std::scoped_lock lock(m_mutex);
        m_cancelled = true;
        m_pendingFlags = 0;
        m_drainScheduled = false;
    }

private:
    // Leaf lock: protects only this state; never nested and never held across callbacks.
    mutable std::mutex m_mutex;
    _Guarded_by_(m_mutex) Flags m_pendingFlags = 0;
    _Guarded_by_(m_mutex) bool m_drainScheduled = false;
    _Guarded_by_(m_mutex) bool m_cancelled = false;
};
