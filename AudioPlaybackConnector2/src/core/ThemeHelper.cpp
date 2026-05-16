#include <pch.h>
#include <core/ThemeHelper.hpp>

/* Member Variables */
/*------------------------------------------------------------------------------------------------------------------*/

wil::srwlock ThemeHelper::s_lock;
std::vector<std::pair<ThemeHelper::ThemeChangedToken, ThemeHelper::ThemeChangedHandler>> ThemeHelper::s_handlers;
ThemeHelper::ThemeChangedToken ThemeHelper::s_nextToken = 1;

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

Theme ThemeHelper::GetSystemTheme() {
    DWORD value = 0, cb = sizeof(value);
    auto status = RegGetValueW(HKEY_CURRENT_USER,
                               LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                               L"SystemUsesLightTheme",
                               RRF_RT_REG_DWORD,
                               nullptr,
                               &value,
                               &cb);
    return (status == ERROR_SUCCESS && value != 0) ? Theme::Light : Theme::Dark;
}

void ThemeHelper::OnSettingChange(HWND, LPARAM lParam) {
    if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
        auto theme = GetSystemTheme();
        static Theme s_lastTheme = theme;
        if (theme == s_lastTheme) return;
        s_lastTheme = theme;

        std::vector<ThemeChangedHandler> copy;
        {
            auto guard = s_lock.lock_shared();
            copy.reserve(s_handlers.size());
            for (auto const& handler : s_handlers) {
                copy.push_back(handler.second);
            }
        }
        for (auto& h : copy)
            h();
    }
}

ThemeHelper::ThemeChangedToken ThemeHelper::AddThemeChangedHandler(ThemeChangedHandler handler) {
    auto guard = s_lock.lock_exclusive();
    auto token = s_nextToken++;
    s_handlers.push_back({token, std::move(handler)});
    return token;
}

void ThemeHelper::RemoveThemeChangedHandler(ThemeChangedToken token) {
    auto guard = s_lock.lock_exclusive();
    std::erase_if(s_handlers, [token](auto const& handler) { return handler.first == token; });
}
