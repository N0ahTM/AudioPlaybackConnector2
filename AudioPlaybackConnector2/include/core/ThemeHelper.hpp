#pragma once

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Theme Helper //////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

enum class Theme { Light,
                   Dark };

class ThemeHelper {
public:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    using ThemeChangedHandler = std::function<void()>;
    using ThemeChangedToken = std::size_t;

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    static Theme GetSystemTheme();
    static void OnSettingChange(HWND hwnd, LPARAM lParam);
    static ThemeChangedToken AddThemeChangedHandler(ThemeChangedHandler handler);
    static void RemoveThemeChangedHandler(ThemeChangedToken token);

private:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    struct HandlerState {
        explicit HandlerState(ThemeChangedHandler handler)
            : Handler(std::move(handler)) {}

        ThemeChangedHandler Handler;
        std::mutex Mutex;
        std::condition_variable Cv;
        std::vector<std::thread::id> InvokingThreads;
        std::size_t InFlight = 0;
        bool Active = true;
    };

    static wil::srwlock s_lock;
    static std::vector<std::pair<ThemeChangedToken, std::shared_ptr<HandlerState>>> s_handlers;
    static ThemeChangedToken s_nextToken;
    static std::optional<Theme> s_lastTheme;
};
