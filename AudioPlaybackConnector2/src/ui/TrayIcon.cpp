#include <pch.h>
#include <ui/TrayIcon.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>
#include <resource.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

static Gdiplus::Color GetSystemBrushColor(const wchar_t* resourceKey) {
    try {
        auto resources = winrt::Microsoft::UI::Xaml::Application::Current().Resources();
        auto obj = resources.Lookup(winrt::box_value(resourceKey));
        auto brush = obj.as<winrt::Microsoft::UI::Xaml::Media::SolidColorBrush>();
        auto c = brush.Color();
        return Gdiplus::Color(c.A, c.R, c.G, c.B);
    } catch (...) {
        return Gdiplus::Color(128, 128, 128);
    }
}

static HICON CreateHIconFromBitmap(Gdiplus::Bitmap& bitmap) {
    int w = bitmap.GetWidth();
    int h = bitmap.GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

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

    // Copy ARGB pixels from GDI+ bitmap.
    // GDI+ may expose a stride that does not match width*4, so copy row-by-row.
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bitmapData;
    Gdiplus::Status status = bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData);
    if (status != Gdiplus::Ok) return nullptr;
    auto cleanupBits = wil::scope_exit([&]() { bitmap.UnlockBits(&bitmapData); });

    if (!bitmapData.Scan0) return nullptr;
    const int sourceStride = bitmapData.Stride;
    if (sourceStride == 0) return nullptr;

    const size_t rowBytes = static_cast<size_t>(w) * 4;
    const auto srcBase = static_cast<std::byte const*>(bitmapData.Scan0);
    auto dstBase = static_cast<std::byte*>(dibBits);

    if (sourceStride > 0) {
        if (static_cast<size_t>(sourceStride) < rowBytes) return nullptr;
        for (int y = 0; y < h; ++y) {
            auto const* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(sourceStride);
            auto* dstRow = dstBase + static_cast<size_t>(y) * rowBytes;
            memcpy(dstRow, srcRow, rowBytes);
        }
    } else {
        const size_t strideAbs = static_cast<size_t>(-sourceStride);
        if (strideAbs < rowBytes) return nullptr;
        for (int y = 0; y < h; ++y) {
            auto const* srcRow = srcBase + static_cast<size_t>(h - 1 - y) * strideAbs;
            auto* dstRow = dstBase + static_cast<size_t>(y) * rowBytes;
            memcpy(dstRow, srcRow, rowBytes);
        }
    }

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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    m_nid.hIcon = m_hIdle[GetBestIconSizeIndex()].get();
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

void TrayIcon::UpdateTheme() {
    CreateAllIcons();
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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TrayIcon::CreateAllIcons() {
    auto baseImage = LoadBitmapResource(GetModuleHandleW(nullptr), IDB_TRAY_ICON);
    if (!baseImage) {
        DebugTrace(L"[TrayIcon] ERROR: LoadBitmapResource failed");
        return;
    }

    auto base = GetSystemBrushColor(L"TextFillColorPrimaryBrush");
    auto success = GetSystemBrushColor(L"SystemFillColorSuccessBrush");
    auto critical = GetSystemBrushColor(L"SystemFillColorCriticalBrush");
    auto caution = GetSystemBrushColor(L"SystemFillColorCautionBrush");
    auto cautionBackground = GetSystemBrushColor(L"SystemFillColorCautionBackgroundBrush");

    decltype(m_hIdle) idle{};
    decltype(m_hConnected) connected{};
    decltype(m_hError) error{};
    decltype(m_hConnecting1) connecting1{};
    decltype(m_hConnecting2) connecting2{};

    for (int i = 0; i < SIZE_COUNT; ++i) {
        int size = SIZES[i];
        try {
            idle[i] = CreateIconFromImage(*baseImage, size, base);
            connected[i] = CreateIconFromImage(*baseImage, size, success);
            error[i] = CreateIconFromImage(*baseImage, size, critical);
            connecting1[i] = CreateIconFromImage(*baseImage, size, caution);
            connecting2[i] = CreateIconFromImage(*baseImage, size, cautionBackground);
        } catch (...) {
            idle[i].reset();
            connected[i].reset();
            error[i].reset();
            connecting1[i].reset();
            connecting2[i].reset();
        }
    }

    auto guard = m_lock.lock_exclusive();
    for (int i = 0; i < SIZE_COUNT; ++i) {
        m_hIdle[i] = std::move(idle[i]);
        m_hConnected[i] = std::move(connected[i]);
        m_hError[i] = std::move(error[i]);
        m_hConnecting1[i] = std::move(connecting1[i]);
        m_hConnecting2[i] = std::move(connecting2[i]);
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
    TrayIconState state;
    bool connectingFrame;
    auto guard = m_lock.lock_exclusive();
    state = m_state;
    connectingFrame = m_connectingFrame;
    HICON hIcon = nullptr;
    switch (state) {
        case TrayIconState::Idle: hIcon = m_hIdle[sizeIdx].get(); break;
        case TrayIconState::Connected: hIcon = m_hConnected[sizeIdx].get(); break;
        case TrayIconState::Error: hIcon = m_hError[sizeIdx].get(); break;
        case TrayIconState::Connecting:
            hIcon = connectingFrame ? m_hConnecting2[sizeIdx].get() : m_hConnecting1[sizeIdx].get();
            break;
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

        Gdiplus::ColorMatrix matrix = {{
            {r, 0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, g, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, b, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        }};

        Gdiplus::ImageAttributes attrs;
        attrs.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

        Gdiplus::RectF destRect(0.0f, 0.0f, static_cast<Gdiplus::REAL>(size), static_cast<Gdiplus::REAL>(size));
        graphics.DrawImage(&source,
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
