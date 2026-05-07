#include <pch.h>
#include <ui/TrayIcon.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>
#include <resource.h>

/* Helpers */
/*------------------------------------------------------------------------------------------------------------------*/

static constexpr int ThemeIndexFromLightBackground(bool lightBackground) {
    return lightBackground ? static_cast<int>(TrayThemeIndex::Light) : static_cast<int>(TrayThemeIndex::Dark);
}

static HICON CreateHIconFromBitmap(Gdiplus::Bitmap& bitmap) {
    int w = bitmap.GetWidth();
    int h = bitmap.GetHeight();

    // Use BITMAPV5HEADER with explicit alpha mask so CreateIconIndirect
    // preserves per-pixel alpha. A black 1bpp mask tells Windows to use
    // the alpha channel from the 32bpp color bitmap instead of the mask.
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = w;
    bi.bV5Height = -h; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    if (!hdc) return nullptr;
    auto cleanupHdc = wil::scope_exit([hdc]() { ReleaseDC(nullptr, hdc); });

    void* dibBits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hbmColor || !dibBits) return nullptr;
    wil::unique_hbitmap ubmColor(hbmColor);

    // Monochrome mask: all black (0) -> Windows uses alpha from color bitmap
    HBITMAP hbmMask = CreateBitmap(w, h, 1, 1, nullptr);
    if (!hbmMask) return nullptr;
    wil::unique_hbitmap ubmMask(hbmMask);

    // Fill mask with black
    wil::unique_hdc maskDC(CreateCompatibleDC(hdc));
    if (!maskDC) return nullptr;
    HBITMAP oldMask = static_cast<HBITMAP>(SelectObject(maskDC.get(), ubmMask.get()));
    auto cleanupMask = wil::scope_exit([&]() { SelectObject(maskDC.get(), oldMask); });
    THROW_LAST_ERROR_IF(!PatBlt(maskDC.get(), 0, 0, w, h, BLACKNESS));

    // Copy ARGB pixels from GDI+ bitmap
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bitmapData;
    Gdiplus::Status status = bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData);
    if (status != Gdiplus::Ok) return nullptr;
    auto cleanupBits = wil::scope_exit([&]() { bitmap.UnlockBits(&bitmapData); });
    memcpy(dibBits, bitmapData.Scan0, static_cast<size_t>(w) * h * 4);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = ubmMask.get();
    ii.hbmColor = ubmColor.get();

    return CreateIconIndirect(&ii);
}

static std::unique_ptr<Gdiplus::Bitmap> LoadBitmapResource(HINSTANCE hInst, int resId) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return nullptr;

    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return nullptr;

    DWORD size = SizeofResource(hInst, hRes);
    void* pData = LockResource(hData);
    if (!pData || size == 0) return nullptr;

    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(static_cast<const BYTE*>(pData), size));
    if (!stream) return nullptr;

    auto bitmap = std::unique_ptr<Gdiplus::Bitmap>(Gdiplus::Bitmap::FromStream(stream.get()));
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
    return bitmap;
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayIcon::Initialize(HWND hwnd, UINT callbackMessage) {
    m_hwnd = hwnd;
    m_callbackMsg = callbackMessage;
    m_guid = c_trayGuid;

    CreateAllIcons();

    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = hwnd;
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    m_nid.uCallbackMessage = callbackMessage;
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    m_nid.hIcon = m_hIdle[0][GetBestIconSizeIndex()].get();
    m_nid.guidItem = m_guid;

    m_niid.cbSize = sizeof(m_niid);
    m_niid.hWnd = hwnd;
    m_niid.uID = 0;
    m_niid.guidItem = m_guid;

    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        DebugTrace(L"[TrayIcon] ERROR: Shell_NotifyIconW(NIM_ADD) failed");
        return;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
}

void TrayIcon::SetState(TrayIconState state) {
    {
        auto guard = m_lock.lock_exclusive();
        m_state = state;
    }
    RefreshIcon();
}

void TrayIcon::SetTooltip(std::wstring_view text) {
    std::wstring tmp(text);
    auto guard = m_lock.lock_exclusive();
    wcsncpy_s(m_nid.szTip, tmp.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void TrayIcon::ShowNotification(std::wstring_view title, std::wstring_view text, TrayNotificationType type) {
    std::wstring t(title);
    std::wstring m(text);
    auto guard = m_lock.lock_exclusive();
    wcsncpy_s(m_nid.szInfoTitle, t.c_str(), _TRUNCATE);
    wcsncpy_s(m_nid.szInfo, m.c_str(), _TRUNCATE);

    m_nid.uFlags |= NIF_INFO;
    switch (type) {
        case TrayNotificationType::Info: m_nid.dwInfoFlags = NIIF_INFO; break;
        case TrayNotificationType::Warning: m_nid.dwInfoFlags = NIIF_WARNING; break;
        case TrayNotificationType::Error: m_nid.dwInfoFlags = NIIF_ERROR; break;
    }

    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    m_nid.uFlags &= ~NIF_INFO;
}

void TrayIcon::UpdateTheme(bool light) {
    {
        auto guard = m_lock.lock_exclusive();
        m_lightTheme = light;
    }
    RefreshIcon();
}

void TrayIcon::ToggleConnectingFrame() {
    {
        auto guard = m_lock.lock_exclusive();
        if (m_state != TrayIconState::Connecting) return;
        m_connectingFrame = !m_connectingFrame;
    }
    RefreshIcon();
}

std::optional<RECT> TrayIcon::GetIconRect() const {
    RECT rc{};
    auto guard = m_lock.lock_shared();
    auto hr = Shell_NotifyIconGetRect(&m_niid, &rc);
    if (FAILED(hr)) return std::nullopt;
    return rc;
}

void TrayIcon::Reregister() {
    auto guard = m_lock.lock_exclusive();
    Shell_NotifyIconW(NIM_ADD, &m_nid);
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
}

void TrayIcon::Remove() {
    auto guard = m_lock.lock_exclusive();
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void TrayIcon::CreateAllIcons() {
    auto baseImage = LoadBitmapResource(GetModuleHandleW(nullptr), IDB_TRAY_ICON);
    if (!baseImage) {
        DebugTrace(L"[TrayIcon] ERROR: LoadBitmapResource failed");
        return;
    }

    for (int i = 0; i < SIZE_COUNT; ++i) {
        int size = SIZES[i];
        try {
            m_hIdle[0][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(32, 32, 32));
            m_hIdle[1][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(255, 255, 255));
            m_hConnected[0][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(159, 216, 159));
            m_hConnected[1][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(159, 216, 159));
            m_hError[0][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(180, 0, 0));
            m_hError[1][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(255, 80, 80));
            m_hConnecting1[0][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(200, 100, 0));
            m_hConnecting1[1][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(255, 160, 0));
            m_hConnecting2[0][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(255, 180, 50));
            m_hConnecting2[1][i] = CreateIconFromImage(*baseImage, size, Gdiplus::Color(255, 200, 100));
        } catch (...) {
            m_hIdle[0][i].reset();
            m_hIdle[1][i].reset();
            m_hConnected[0][i].reset();
            m_hConnected[1][i].reset();
            m_hError[0][i].reset();
            m_hError[1][i].reset();
            m_hConnecting1[0][i].reset();
            m_hConnecting1[1][i].reset();
            m_hConnecting2[0][i].reset();
            m_hConnecting2[1][i].reset();
        }
    }
}

int TrayIcon::GetBestIconSizeIndex() {
    int targetSize = GetSystemMetrics(SM_CXSMICON);
    if (targetSize == 0) targetSize = 16;

    for (int i = 0; i < SIZE_COUNT; ++i) {
        if (SIZES[i] >= targetSize) {
            return i;
        }
    }
    return SIZE_COUNT - 1;
}

void TrayIcon::RefreshIcon() {
    int sizeIdx = GetBestIconSizeIndex();
    int idx;
    TrayIconState state;
    bool connectingFrame;
    auto guard = m_lock.lock_exclusive();
    idx = ThemeIndexFromLightBackground(m_lightTheme);
    state = m_state;
    connectingFrame = m_connectingFrame;
    HICON hIcon = nullptr;
    switch (state) {
        case TrayIconState::Idle: hIcon = m_hIdle[idx][sizeIdx].get(); break;
        case TrayIconState::Connected: hIcon = m_hConnected[idx][sizeIdx].get(); break;
        case TrayIconState::Error: hIcon = m_hError[idx][sizeIdx].get(); break;
        case TrayIconState::Connecting: hIcon = connectingFrame ? m_hConnecting2[idx][sizeIdx].get() : m_hConnecting1[idx][sizeIdx].get(); break;
    }
    m_nid.hIcon = hIcon;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

wil::unique_hicon TrayIcon::CreateIconFromImage(Gdiplus::Bitmap& source, int size, Gdiplus::Color color) {
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        float r = color.GetR() / 255.0f;
        float g = color.GetG() / 255.0f;
        float b = color.GetB() / 255.0f;

        Gdiplus::ColorMatrix matrix = {
            r, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, g, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, b, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

        Gdiplus::ImageAttributes attrs;
        attrs.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

        Gdiplus::RectF destRect(0.0f, 0.0f, static_cast<Gdiplus::REAL>(size), static_cast<Gdiplus::REAL>(size));
        graphics.DrawImage(
            &source,
            destRect,
            0.0f,
            0.0f,
            static_cast<Gdiplus::REAL>(source.GetWidth()),
            static_cast<Gdiplus::REAL>(source.GetHeight()),
            Gdiplus::UnitPixel,
            &attrs);
    }

    HICON hIcon = CreateHIconFromBitmap(bitmap);
    return wil::unique_hicon(hIcon);
}
