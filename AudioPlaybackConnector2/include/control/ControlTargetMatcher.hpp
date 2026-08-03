#pragma once

#include <algorithm>
#include <cwctype>
#include <span>
#include <string>
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

namespace target_matcher_details {
inline std::wstring Lower(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (wchar_t character : value)
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    return lowered;
}

inline bool Equals(std::wstring_view lhs, std::wstring_view rhs) {
    return lhs.size() == rhs.size() && Lower(lhs) == Lower(rhs);
}

inline bool Contains(std::wstring_view value, std::wstring_view query) {
    return !query.empty() && Lower(value).find(Lower(query)) != std::wstring::npos;
}

inline std::wstring NormalizeHex(std::wstring_view value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (wchar_t character : value) {
        if ((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F')) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return normalized;
}
} // namespace target_matcher_details

inline AutoTargetMatches FindAutoTargetMatches(std::span<TargetCandidateView const> candidates,
                                               std::wstring_view query) {
    using namespace target_matcher_details;
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

    if (collect(AutoTargetMatchRank::ExactId, [&](auto const& candidate) { return Equals(candidate.Id, query); })) {
        return result;
    }
    if (collect(AutoTargetMatchRank::ExactAliasOrName, [&](auto const& candidate) {
            return Equals(candidate.Alias, query) || Equals(candidate.Name, query);
        })) {
        return result;
    }

    const auto queryHex = NormalizeHex(query);
    if (queryHex.size() >= 6 && collect(AutoTargetMatchRank::MacFragment, [&](auto const& candidate) {
            return NormalizeHex(candidate.Id).find(queryHex) != std::wstring::npos;
        })) {
        return result;
    }
    (void)collect(AutoTargetMatchRank::AliasOrNameSubstring, [&](auto const& candidate) {
        return Contains(candidate.Alias, query) || Contains(candidate.Name, query);
    });
    return result;
}

} // namespace apc::control
