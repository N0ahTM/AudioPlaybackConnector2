#include <pch.h>
#include <core/ThemeHelper.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Static State //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct ThemeHelper::StaticState {
    wil::srwlock lock;
    std::vector<std::pair<ThemeHelper::ThemeChangedToken, std::shared_ptr<ThemeHelper::HandlerState>>> handlers;
    ThemeHelper::ThemeChangedToken nextToken = 1;
    std::optional<Theme> lastTheme;
};

ThemeHelper::StaticState& ThemeHelper::GetStaticState() {
    static StaticState state;
    return state;
}

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

        auto& state = GetStaticState();
        std::vector<std::shared_ptr<HandlerState>> copy;
        {
            auto guard = state.lock.lock_exclusive();
            if (!state.lastTheme) {
                state.lastTheme = theme;
                return;
            }
            if (theme == *state.lastTheme) return;
            state.lastTheme = theme;

            copy.reserve(state.handlers.size());
            for (auto const& handler : state.handlers) {
                copy.push_back(handler.second);
            }
        }

        for (auto& handlerState : copy) {
            ThemeChangedHandler handler;
            {
                std::lock_guard guard(handlerState->Mutex);
                if (!handlerState->Active || !handlerState->Handler) continue;
                handler = handlerState->Handler;
                ++handlerState->InFlight;
                handlerState->InvokingThreads.push_back(std::this_thread::get_id());
            }

            try {
                handler();
            } catch (...) {
                DebugTrace(L"[ThemeHelper] Theme changed handler failed");
            }

            {
                std::lock_guard guard(handlerState->Mutex);
                auto currentThread = std::this_thread::get_id();
                auto iter = std::ranges::find(handlerState->InvokingThreads, currentThread);
                if (iter != handlerState->InvokingThreads.end()) {
                    handlerState->InvokingThreads.erase(iter);
                }
                if (handlerState->InFlight > 0) {
                    --handlerState->InFlight;
                }
                if (handlerState->InFlight == 0) {
                    handlerState->Cv.notify_all();
                }
            }
        }
    }
}

ThemeHelper::ThemeChangedToken ThemeHelper::AddThemeChangedHandler(ThemeChangedHandler handler) {
    auto& state = GetStaticState();
    auto guard = state.lock.lock_exclusive();
    auto token = state.nextToken++;
    state.handlers.push_back({token, std::make_shared<HandlerState>(std::move(handler))});
    return token;
}

void ThemeHelper::RemoveThemeChangedHandler(ThemeChangedToken token) {
    auto& state = GetStaticState();
    std::shared_ptr<HandlerState> handlerState;
    {
        auto guard = state.lock.lock_exclusive();
        auto iter =
            std::ranges::find_if(state.handlers, [token](auto const& handler) { return handler.first == token; });
        if (iter == state.handlers.end()) return;
        handlerState = iter->second;
        state.handlers.erase(iter);
    }

    std::unique_lock guard(handlerState->Mutex);
    handlerState->Active = false;
    handlerState->Handler = nullptr;
    auto currentThread = std::this_thread::get_id();
    if (std::ranges::find(handlerState->InvokingThreads, currentThread) == handlerState->InvokingThreads.end()) {
        handlerState->Cv.wait(guard, [&] { return handlerState->InFlight == 0; });
    }
}
