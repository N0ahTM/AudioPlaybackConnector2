#pragma once

#include <atomic>

class ControlUiActionGate {
public:
    enum class Phase { Pending, Running, Cancelled, Completed };
    enum class Result { Failed, Succeeded, Indeterminate };

    [[nodiscard]] bool TryBegin() noexcept {
        auto expected = Phase::Pending;
        return m_phase.compare_exchange_strong(expected, Phase::Running);
    }

    void Complete(bool succeeded) noexcept {
        m_succeeded.store(succeeded);
        m_phase.store(Phase::Completed);
    }

    [[nodiscard]] Result CancelOrClassify() noexcept {
        auto expected = Phase::Pending;
        if (m_phase.compare_exchange_strong(expected, Phase::Cancelled)) return Result::Failed;
        return Classify(expected);
    }

    [[nodiscard]] Result CurrentResult() const noexcept { return Classify(m_phase.load()); }
    [[nodiscard]] Phase CurrentPhase() const noexcept { return m_phase.load(); }

private:
    [[nodiscard]] Result Classify(Phase phase) const noexcept {
        if (phase == Phase::Completed) return m_succeeded.load() ? Result::Succeeded : Result::Failed;
        return phase == Phase::Running ? Result::Indeterminate : Result::Failed;
    }

    std::atomic<Phase> m_phase = Phase::Pending;
    std::atomic<bool> m_succeeded = false;
};
