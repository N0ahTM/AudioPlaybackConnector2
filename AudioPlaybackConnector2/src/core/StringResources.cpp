#include <pch.h>
#include <core/StringResources.hpp>
#include <nlohmann/json.hpp>
#include <util/Util.hpp>
#include <resource.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void StringResources::Initialize(HINSTANCE hInst, std::wstring_view language, util::LogSink const& log) {
    LANGID langId = GetUserDefaultUILanguage();
    int resId = IDR_STRINGS_EN;
    if (!language.empty() && language != L"system") {
        if (language == L"de")
            resId = IDR_STRINGS_DE;
        else if (language == L"fr")
            resId = IDR_STRINGS_FR;
        else if (language == L"es")
            resId = IDR_STRINGS_ES;
        else if (language == L"ja")
            resId = IDR_STRINGS_JA;
        else if (language == L"ko")
            resId = IDR_STRINGS_KO;
        else if (language == L"zh_hant")
            resId = IDR_STRINGS_ZH_HANT;
        else if (language == L"zh_hans")
            resId = IDR_STRINGS_ZH_HANS;
    } else {
        switch (PRIMARYLANGID(langId)) {
            case LANG_GERMAN: resId = IDR_STRINGS_DE; break;
            case LANG_FRENCH: resId = IDR_STRINGS_FR; break;
            case LANG_SPANISH: resId = IDR_STRINGS_ES; break;
            case LANG_JAPANESE: resId = IDR_STRINGS_JA; break;
            case LANG_KOREAN: resId = IDR_STRINGS_KO; break;
            case LANG_CHINESE: {
                const auto subLanguage = SUBLANGID(langId);
                if (subLanguage == SUBLANG_CHINESE_TRADITIONAL || subLanguage == SUBLANG_CHINESE_HONGKONG ||
                    subLanguage == SUBLANG_CHINESE_MACAU)
                    resId = IDR_STRINGS_ZH_HANT;
                else
                    resId = IDR_STRINGS_ZH_HANS;
                break;
            }
        }
    }

    const auto load = [&](int resourceId) -> std::optional<decltype(m_map)> {
        const auto data = util::LoadResourceData(hInst, resourceId, L"JSON");
        if (!data) return std::nullopt;
        try {
            const std::string_view jsonView(reinterpret_cast<const char*>(data->data()), data->size());
            const auto json = nlohmann::json::parse(jsonView);
            if (!json.is_object()) return std::nullopt;
            decltype(m_map) strings;
            for (auto const& [key, text] : json.items()) {
                if (key.empty() || !text.is_string()) return std::nullopt;
                strings.emplace(key, std::wstring(winrt::to_hstring(text.get_ref<std::string const&>())));
            }
            return strings;
        } catch (...) {
            log.Trace(L"[StringResources] Initialize ERROR: failed to parse strings JSON resource {0}", resourceId);
            return std::nullopt;
        }
    };

    // Build the complete candidate before taking the publication lock. Parsing,
    // logging and destruction of the previous language run outside the lock.
    auto candidate = load(IDR_STRINGS_EN);
    if (!candidate) return;
    if (resId != IDR_STRINGS_EN) {
        if (auto overlay = load(resId)) {
            for (auto& [key, value] : *overlay)
                candidate->insert_or_assign(key, std::move(value));
        }
    }
    {
        auto guard = m_lock.lock_exclusive();
        m_map.swap(*candidate);
    }
}

std::wstring StringResources::Get(std::string_view key) const {
    auto guard = m_lock.lock_shared();
    auto it = m_map.find(key);
    if (it != m_map.end()) return it->second;
    return L"";
}
