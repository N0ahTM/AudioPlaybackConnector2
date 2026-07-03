#pragma once

#include <string_view>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Icon State ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class TrayIconState { Idle, Connecting, Connected, Error };

[[nodiscard]] std::wstring_view TrayIconStateToString(TrayIconState state) noexcept;

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
    [[nodiscard]] TrayIconState State() const;
    void Reregister();
    void SetState(TrayIconState state);
    void SetTooltip(std::wstring_view text);
    void UpdateTheme();
    void AdvanceConnectingFrame();
    std::optional<RECT> GetIconRect() const;
    void Remove();

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void CreateAllIcons();
    void RefreshIcon(bool logSuccess);
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
    uint8_t m_connectingFrame = 0;
    bool m_initialized = false;
    bool m_registered = false;

    static constexpr GUID c_trayGuid = {0xcf5eeb74, 0x90fb, 0x441e, {0xbf, 0x17, 0x72, 0x84, 0x02, 0x0e, 0xf0, 0x5c}};
    static constexpr int SIZE_COUNT = 4;
    static constexpr int CONNECTING_FRAME_COUNT = 8;
    static constexpr int SIZES[SIZE_COUNT] = {16, 20, 24, 32};
    wil::unique_hicon m_hIdle[SIZE_COUNT];
    wil::unique_hicon m_hConnected[SIZE_COUNT];
    wil::unique_hicon m_hError[SIZE_COUNT];
    wil::unique_hicon m_hConnecting[CONNECTING_FRAME_COUNT][SIZE_COUNT];

    GUID m_guid{};
};
