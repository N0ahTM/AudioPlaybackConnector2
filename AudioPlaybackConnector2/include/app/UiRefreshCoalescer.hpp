#pragma once

#include <cstdint>
#include <mutex>

class UiRefreshCoalescer {
public:
    using Flags = std::uint32_t;

    [[nodiscard]] bool Request(Flags flags) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled) return false;
        m_pendingFlags |= flags;
        if (m_drainScheduled) {
            m_requestArrivedAfterSchedule = true;
            return false;
        }
        m_drainScheduled = true;
        m_requestArrivedAfterSchedule = false;
        return true;
    }

    [[nodiscard]] Flags BeginDrain() noexcept {
        std::scoped_lock lock(m_mutex);
        auto flags = m_pendingFlags;
        m_pendingFlags = 0;
        m_requestArrivedAfterSchedule = false;
        return flags;
    }

    [[nodiscard]] bool CompleteDrain() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled) return false;
        if (m_pendingFlags != 0) {
            m_requestArrivedAfterSchedule = false;
            return true;
        }
        m_drainScheduled = false;
        m_requestArrivedAfterSchedule = false;
        return false;
    }

    [[nodiscard]] bool ScheduleFailed() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled) return false;

        m_drainScheduled = false;
        if (!m_requestArrivedAfterSchedule) return false;

        m_requestArrivedAfterSchedule = false;
        m_drainScheduled = true;
        return true;
    }

    void AbandonSchedule() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_cancelled) return;
        m_drainScheduled = false;
        m_requestArrivedAfterSchedule = false;
    }

    void Cancel() noexcept {
        std::scoped_lock lock(m_mutex);
        m_cancelled = true;
        m_pendingFlags = 0;
        m_drainScheduled = false;
        m_requestArrivedAfterSchedule = false;
    }

    [[nodiscard]] bool DrainScheduled() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_drainScheduled;
    }

private:
    mutable std::mutex m_mutex;
    Flags m_pendingFlags = 0;
    bool m_drainScheduled = false;
    bool m_requestArrivedAfterSchedule = false;
    bool m_cancelled = false;
};
