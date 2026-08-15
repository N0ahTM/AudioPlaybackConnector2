#include <services/AutomaticUpdateWindow.hpp>

bool AutomaticUpdateWindow::Update(bool allowed, bool stopping, TimePoint now, Clock::duration stableDelay) noexcept {
    if (stopping || !allowed) {
        Reset();
        return false;
    }
    if (!m_allowedSince || now < *m_allowedSince) m_allowedSince = now;
    return now - *m_allowedSince >= stableDelay;
}

AutomaticUpdateWindow::Clock::duration AutomaticUpdateWindow::Remaining(TimePoint now,
                                                                        Clock::duration stableDelay) const noexcept {
    if (!m_allowedSince || now < *m_allowedSince) return stableDelay;
    const auto elapsed = now - *m_allowedSince;
    return elapsed >= stableDelay ? Clock::duration::zero() : stableDelay - elapsed;
}

void AutomaticUpdateWindow::Reset() noexcept {
    m_allowedSince.reset();
}
