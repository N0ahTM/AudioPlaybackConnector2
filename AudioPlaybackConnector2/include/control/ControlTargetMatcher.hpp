#pragma once

#include <util/Text.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace apc::control {

struct TargetCandidateView {
    std::wstring_view Id;
    std::wstring_view Name;
    std::wstring_view Alias;
};

enum class AutoTargetMatchRank { None, ExactId, ExactAliasOrName, MacFragment, AliasOrNameSubstring };

struct AutoTargetMatches {
    AutoTargetMatchRank Rank = AutoTargetMatchRank::None;
    std::vector<std::size_t> Indices;
};

inline AutoTargetMatches FindAutoTargetMatches(std::span<TargetCandidateView const> candidates,
                                               std::wstring_view query) {
    if (query.empty()) return {};

    AutoTargetMatches result;
    const auto collect = [&](AutoTargetMatchRank rank, auto predicate) {
        result.Indices.clear();
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (predicate(candidates[index])) result.Indices.push_back(index);
        }
        if (!result.Indices.empty()) result.Rank = rank;
        return !result.Indices.empty();
    };

    if (collect(AutoTargetMatchRank::ExactId,
                [&](auto const& candidate) { return util::EqualsIgnoreCase(candidate.Id, query); })) {
        return result;
    }
    if (collect(AutoTargetMatchRank::ExactAliasOrName, [&](auto const& candidate) {
            return util::EqualsIgnoreCase(candidate.Alias, query) || util::EqualsIgnoreCase(candidate.Name, query);
        })) {
        return result;
    }

    const auto queryHex = util::NormalizeHex(query);
    if (queryHex.size() >= 6 && collect(AutoTargetMatchRank::MacFragment, [&](auto const& candidate) {
            return util::NormalizeHex(candidate.Id).find(queryHex) != std::wstring::npos;
        })) {
        return result;
    }
    (void)collect(AutoTargetMatchRank::AliasOrNameSubstring, [&](auto const& candidate) {
        return util::ContainsIgnoreCase(candidate.Alias, query) || util::ContainsIgnoreCase(candidate.Name, query);
    });
    return result;
}

} // namespace apc::control
