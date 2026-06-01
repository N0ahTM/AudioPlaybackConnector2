#pragma once

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Icon State ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class TrayIconState { Idle, Connecting, Connected, Error };
enum class TrayNotificationType { Info, Warning, Error };

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Icon /////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class TrayIcon {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Initialize(HWND hwnd, UINT callbackMessage);
    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    void Reregister();
    void SetState(TrayIconState state);
    void SetTooltip(std::wstring_view text);
    void ShowNotification(std::wstring_view title, std::wstring_view text, TrayNotificationType type);
    void UpdateTheme();
    void ToggleConnectingFrame();
    std::optional<RECT> GetIconRect() const;
    void Remove();

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void CreateAllIcons();
    void RefreshIcon();
    static wil::unique_hicon CreateIconFromImage(Gdiplus::Bitmap& source, int size, Gdiplus::Color color);
    static int GetBestIconSizeIndex();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    HWND m_hwnd = nullptr;
    UINT m_callbackMsg = 0;
    NOTIFYICONDATAW m_nid{};
    NOTIFYICONIDENTIFIER m_niid{};

    mutable wil::srwlock m_lock;
    TrayIconState m_state = TrayIconState::Idle;
    bool m_connectingFrame = false;
    bool m_initialized = false;

    static constexpr GUID c_trayGuid = {0xcf5eeb74, 0x90fb, 0x441e, {0xbf, 0x17, 0x72, 0x84, 0x02, 0x0e, 0xf0, 0x5c}};
    static constexpr int SIZE_COUNT = 4;
    static constexpr int SIZES[SIZE_COUNT] = {16, 20, 24, 32};
    wil::unique_hicon m_hIdle[SIZE_COUNT];
    wil::unique_hicon m_hConnected[SIZE_COUNT];
    wil::unique_hicon m_hError[SIZE_COUNT];
    wil::unique_hicon m_hConnecting1[SIZE_COUNT];
    wil::unique_hicon m_hConnecting2[SIZE_COUNT];

    GUID m_guid{};
};
