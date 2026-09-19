#include <core/ThemeHelper.hpp>
#include <windows.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Windows Theme Query ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

Theme GetSystemTheme() noexcept {
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
