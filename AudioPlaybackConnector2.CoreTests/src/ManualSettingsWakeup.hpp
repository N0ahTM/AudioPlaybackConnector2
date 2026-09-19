#pragma once

#include <core/SettingsStoreWakeup.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
#include <utility>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Manual Persistence Clock //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ManualSettingsWakeup final : public SettingsStoreWakeup {
public:
    struct WaitGate {
        std::binary_semaphore Entered{0};
        std::binary_semaphore Release{0};
    };

    Clock::time_point Now() const noexcept override {
        std::scoped_lock lock(m_mutex);
        return m_now;
    }
    std::uint64_t Version() const noexcept override {
        std::scoped_lock lock(m_mutex);
        return m_version;
    }
    void Notify() noexcept override {
        {
            std::scoped_lock lock(m_mutex);
            ++m_version;
        }
        m_changed.notify_all();
    }
    void Wait(std::uint64_t version, std::optional<Clock::time_point> deadline) noexcept override {
        std::unique_lock lock(m_mutex);
        if (auto gate = std::exchange(m_nextWaitGate, {})) {
            lock.unlock();
            gate->Entered.release();
            gate->Release.acquire();
            lock.lock();
        }
        auto ready = [&] { return m_version != version || (deadline && m_now >= *deadline); };
        if (ready()) return;
        m_waiting = true;
        m_waitVersion = version;
        m_deadline = deadline;
        m_parked.notify_all();
        m_changed.wait(lock, ready);
        m_waiting = false;
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Test Control //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::uint64_t Advance(Clock::duration amount) {
        std::uint64_t version;
        {
            std::scoped_lock lock(m_mutex);
            m_now += amount;
            version = ++m_version;
        }
        m_changed.notify_all();
        return version;
    }
    void BlockNextWait(std::shared_ptr<WaitGate> gate) {
        std::scoped_lock lock(m_mutex);
        m_nextWaitGate = std::move(gate);
    }
    bool WaitUntilParked(std::uint64_t version, std::optional<Clock::time_point> deadline) {
        std::unique_lock lock(m_mutex);
        return m_parked.wait_for(lock, std::chrono::seconds{2}, [&] {
            return m_waiting && m_waitVersion >= version && m_deadline == deadline;
        });
    }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::condition_variable m_parked;
    Clock::time_point m_now{};
    std::optional<Clock::time_point> m_deadline;
    std::uint64_t m_version = 0;
    std::uint64_t m_waitVersion = 0;
    bool m_waiting = false;
    std::shared_ptr<WaitGate> m_nextWaitGate;
};
