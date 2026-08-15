#include <app/AdaptiveScheduleState.hpp>

std::uint64_t AdaptiveScheduleState::Supersede() noexcept {
    ++m_generation;
    if (m_generation == 0) ++m_generation;
    m_win32NotBefore.reset();
    m_active = true;
    return m_generation;
}

bool AdaptiveScheduleState::SetWin32NotBefore(std::uint64_t generation, TimePoint notBefore) noexcept {
    if (generation != m_generation || !m_active) return false;
    m_win32NotBefore = notBefore;
    return true;
}

bool AdaptiveScheduleState::Consume(std::uint64_t generation) noexcept {
    if (generation != m_generation || !m_active) return false;
    m_win32NotBefore.reset();
    m_active = false;
    return true;
}

bool AdaptiveScheduleState::ConsumeWin32IfDue(TimePoint now) noexcept {
    if (!m_active || !m_win32NotBefore || now < *m_win32NotBefore) return false;
    m_win32NotBefore.reset();
    m_active = false;
    return true;
}

std::uint64_t AdaptiveScheduleState::Generation() const noexcept {
    return m_generation;
}
