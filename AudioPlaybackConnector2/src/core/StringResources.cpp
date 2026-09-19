#include <pch.h>
#include <core/StringResources.hpp>
#include <nlohmann/json.hpp>
#include <util/Util.hpp>
#include <resource.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

StringResources& StringResources::Instance() {
    static StringResources s_instance;
    return s_instance;
}

void StringResources::Initialize(HINSTANCE hInst) {
    Initialize(hInst, {});
}

void StringResources::Initialize(HINSTANCE hInst, std::wstring_view language) {
    auto guard = m_lock.lock_exclusive();
    m_hInst = hInst;
    m_map.clear();

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

    const auto load = [this](int resourceId, bool replace) {
        const auto data = util::LoadResourceData(m_hInst, resourceId, L"JSON");
        if (!data) return false;

        try {
            const std::string_view jsonView(reinterpret_cast<const char*>(data->data()), data->size());
            const auto json = nlohmann::json::parse(jsonView);
            if (!json.is_object()) return false;
            for (auto const& [resourceKey, text] : json.items()) {
                if (!text.is_string()) continue;
                auto key = resourceKey;
                auto value = std::wstring(winrt::to_hstring(text.get_ref<std::string const&>()));
                if (replace)
                    m_map.insert_or_assign(std::move(key), std::move(value));
                else
                    m_map.emplace(std::move(key), std::move(value));
            }
            return true;
        } catch (...) {
            DebugTrace(L"[StringResources] Initialize ERROR: failed to parse strings JSON resource {0}", resourceId);
            return false;
        }
    };

    // English is the complete source language. A selected locale overlays it so
    // an incomplete community translation never turns a label into an empty string.
    if (!load(IDR_STRINGS_EN, false)) return;
    if (resId != IDR_STRINGS_EN) load(resId, true);
}

std::wstring StringResources::Get(std::string_view key) const {
    auto guard = m_lock.lock_shared();
    auto it = m_map.find(key);
    if (it != m_map.end()) return it->second;
    return L"";
}
