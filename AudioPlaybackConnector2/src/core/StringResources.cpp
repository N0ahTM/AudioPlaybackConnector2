#include <pch.h>
#include <core/StringResources.hpp>
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
            case LANG_CHINESE:
                if (SUBLANGID(langId) == SUBLANG_CHINESE_TRADITIONAL)
                    resId = IDR_STRINGS_ZH_HANT;
                else
                    resId = IDR_STRINGS_ZH_HANS;
                break;
        }
    }

    auto data = util::LoadResourceData(m_hInst, resId, L"JSON");
    if (!data) {
        data = util::LoadResourceData(m_hInst, IDR_STRINGS_EN, L"JSON");
        if (!data) return;
    }

    try {
        std::string_view jsonView(reinterpret_cast<const char*>(data->data()), data->size());
        auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(jsonView));
        for (auto pair : json) {
            auto key = winrt::to_string(pair.Key());
            if (pair.Value().ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                m_map.emplace(key, std::wstring(pair.Value().GetString()));
        }
    } catch (...) {
        DebugTrace(L"[StringResources] Initialize ERROR: failed to parse strings JSON");
    }
}

std::wstring_view StringResources::Get(std::string_view key) const {
    auto guard = m_lock.lock_shared();
    auto it = m_map.find(key);
    if (it != m_map.end()) return it->second;
    return L"";
}
