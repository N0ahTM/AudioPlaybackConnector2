#pragma once

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Icon State ///////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

enum class TrayIconState { Idle,
                           Connecting,
                           Connected,
                           Error };
enum class TrayNotificationType { Info,
                                  Warning,
                                  Error };
enum class TrayThemeIndex : int { Light = 0,
                                  Dark = 1,
                                  Count };

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Icon /////////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

class TrayIcon {
public:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void Initialize(HWND hwnd, UINT callbackMessage);
    void Reregister();
    void SetState(TrayIconState state);
    void SetTooltip(std::wstring_view text);
    void ShowNotification(std::wstring_view title, std::wstring_view text, TrayNotificationType type);
    void UpdateTheme(bool light);
    void ToggleConnectingFrame();
    std::optional<RECT> GetIconRect() const;
    void Remove();

private:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void CreateAllIcons();
    void RefreshIcon();
    static wil::unique_hicon CreateIconFromImage(Gdiplus::Bitmap& source, int size, Gdiplus::Color color);
    static int GetBestIconSizeIndex();

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    HWND m_hwnd = nullptr;
    UINT m_callbackMsg = 0;
    NOTIFYICONDATAW m_nid{};
    NOTIFYICONIDENTIFIER m_niid{};

    mutable wil::srwlock m_lock;
    TrayIconState m_state = TrayIconState::Idle;
    bool m_lightTheme = false;
    bool m_connectingFrame = false;

    static constexpr GUID c_trayGuid = {0xcf5eeb74, 0x90fb, 0x441e, {0xbf, 0x17, 0x72, 0x84, 0x02, 0x0e, 0xf0, 0x5c}};
    static constexpr int SIZE_COUNT = 4;
    static constexpr int SIZES[SIZE_COUNT] = {16, 20, 24, 32};
    static constexpr int THEME_COUNT = static_cast<int>(TrayThemeIndex::Count);

    wil::unique_hicon m_hIdle[THEME_COUNT][SIZE_COUNT];
    wil::unique_hicon m_hConnected[THEME_COUNT][SIZE_COUNT];
    wil::unique_hicon m_hError[THEME_COUNT][SIZE_COUNT];
    wil::unique_hicon m_hConnecting1[THEME_COUNT][SIZE_COUNT];
    wil::unique_hicon m_hConnecting2[THEME_COUNT][SIZE_COUNT];

    GUID m_guid{};
};
