#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ResumeReconnectAttemptState {
public:
    struct Selection {
        std::vector<std::wstring> Eligible;
        std::vector<std::wstring> Exhausted;
    };

    void BeginCycle(std::vector<std::wstring> activeDeviceIds) {
        m_attemptCounts.clear();
        for (auto& id : activeDeviceIds) {
            if (!id.empty() && std::ranges::find(m_deviceIds, id) == m_deviceIds.end()) {
                m_deviceIds.push_back(std::move(id));
            }
        }
    }

    [[nodiscard]] Selection SelectEligible(unsigned int maximumAttempts) {
        Selection selection;
        for (auto it = m_deviceIds.begin(); it != m_deviceIds.end();) {
            auto const count = m_attemptCounts.contains(*it) ? m_attemptCounts.at(*it) : 0U;
            if (count >= maximumAttempts) {
                selection.Exhausted.push_back(*it);
                m_attemptCounts.erase(*it);
                it = m_deviceIds.erase(it);
            } else {
                selection.Eligible.push_back(*it);
                ++it;
            }
        }
        return selection;
    }

    void RecordAttempts(std::vector<std::wstring> const& attemptedIds) {
        for (auto const& id : attemptedIds) {
            if (std::ranges::find(m_deviceIds, id) != m_deviceIds.end()) ++m_attemptCounts[id];
        }
    }

    [[nodiscard]] bool Acknowledge(std::wstring_view deviceId) {
        auto const previousSize = m_deviceIds.size();
        std::erase(m_deviceIds, std::wstring(deviceId));
        m_attemptCounts.erase(std::wstring(deviceId));
        return m_deviceIds.size() != previousSize;
    }

    void Clear() noexcept {
        m_deviceIds.clear();
        m_attemptCounts.clear();
    }

    [[nodiscard]] bool Empty() const noexcept { return m_deviceIds.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return m_deviceIds.size(); }

private:
    std::vector<std::wstring> m_deviceIds;
    std::unordered_map<std::wstring, unsigned int> m_attemptCounts;
};
