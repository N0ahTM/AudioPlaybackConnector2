#include <pch.h>
#include <core/ThemeHelper.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

wil::srwlock ThemeHelper::s_lock;
std::vector<std::pair<ThemeHelper::ThemeChangedToken, std::shared_ptr<ThemeHelper::HandlerState>>>
    ThemeHelper::s_handlers;
ThemeHelper::ThemeChangedToken ThemeHelper::s_nextToken = 1;
std::optional<Theme> ThemeHelper::s_lastTheme;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    if (lParam &&
        CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
        auto theme = GetSystemTheme();

        std::vector<std::shared_ptr<HandlerState>> copy;
        {
            auto guard = s_lock.lock_exclusive();
            if (!s_lastTheme) {
                s_lastTheme = theme;
                return;
            }
            if (theme == *s_lastTheme) return;
            s_lastTheme = theme;

            copy.reserve(s_handlers.size());
            for (auto const& handler : s_handlers) {
                copy.push_back(handler.second);
            }
        }

        for (auto& state : copy) {
            ThemeChangedHandler handler;
            {
                std::lock_guard guard(state->Mutex);
                if (!state->Active || !state->Handler) continue;
                handler = state->Handler;
                ++state->InFlight;
                state->InvokingThreads.push_back(std::this_thread::get_id());
            }

            try {
                handler();
            } catch (...) {
                DebugTrace(L"[ThemeHelper] Theme changed handler failed");
            }

            {
                std::lock_guard guard(state->Mutex);
                auto currentThread = std::this_thread::get_id();
                auto iter = std::ranges::find(state->InvokingThreads, currentThread);
                if (iter != state->InvokingThreads.end()) {
                    state->InvokingThreads.erase(iter);
                }
                if (state->InFlight > 0) {
                    --state->InFlight;
                }
                if (state->InFlight == 0) {
                    state->Cv.notify_all();
                }
            }
        }
    }
}

ThemeHelper::ThemeChangedToken ThemeHelper::AddThemeChangedHandler(ThemeChangedHandler handler) {
    auto guard = s_lock.lock_exclusive();
    auto token = s_nextToken++;
    s_handlers.push_back({token, std::make_shared<HandlerState>(std::move(handler))});
    return token;
}

void ThemeHelper::RemoveThemeChangedHandler(ThemeChangedToken token) {
    std::shared_ptr<HandlerState> state;
    {
        auto guard = s_lock.lock_exclusive();
        auto iter = std::ranges::find_if(s_handlers, [token](auto const& handler) { return handler.first == token; });
        if (iter == s_handlers.end()) return;
        state = iter->second;
        s_handlers.erase(iter);
    }

    std::unique_lock guard(state->Mutex);
    state->Active = false;
    state->Handler = nullptr;
    auto currentThread = std::this_thread::get_id();
    if (std::ranges::find(state->InvokingThreads, currentThread) == state->InvokingThreads.end()) {
        state->Cv.wait(guard, [&] { return state->InFlight == 0; });
    }
}
