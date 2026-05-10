#pragma once

#include <chrono>
#include <tuple>

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Event Template ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

template <typename... Args>
class Event {
public:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    using Handler = std::function<void(Args...)>;
    using HandlerId = std::size_t;

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    HandlerId operator+=(Handler handler) {
        auto guard = m_lock.lock_exclusive();
        HandlerId id = m_nextId++;
        m_handlers.push_back({id, std::make_shared<Handler>(std::move(handler))});
        return id;
    }

    void operator-=(HandlerId id) {
        auto guard = m_lock.lock_exclusive();
        m_handlers.erase(std::remove_if(m_handlers.begin(), m_handlers.end(), [id](const auto& pair) { return pair.first == id; }), m_handlers.end());
    }

    template <typename... CallArgs>
    void operator()(CallArgs&&... args) const {
        auto snapshot = std::make_tuple(std::decay_t<CallArgs>(std::forward<CallArgs>(args))...);
        std::vector<std::shared_ptr<Handler>> copy;
        {
            auto guard = m_lock.lock_shared();
            copy.reserve(m_handlers.size());
            for (auto& h : m_handlers) {
                copy.push_back(h.second);
            }
        }
        for (auto& h : copy) {
            std::apply([&](auto const&... values) {
                (*h)(values...);
            },
                       snapshot);
        }
    }

    void clear() {
        auto guard = m_lock.lock_exclusive();
        m_handlers.clear();
    }

private:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::vector<std::pair<HandlerId, std::shared_ptr<Handler>>> m_handlers;
    HandlerId m_nextId = 1;
};

/* String Helpers */
/*------------------------------------------------------------------------------------------------------------------*/

namespace util {

inline std::wstring Utf8ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for MultiByteToWideChar");
    }
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(len == 0);
    std::wstring out(len, L'\0');
    THROW_LAST_ERROR_IF(0 == MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), len));
    return out;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for WideCharToMultiByte");
    }
    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(len == 0);
    std::string out(len, '\0');
    THROW_LAST_ERROR_IF(0 == WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), out.data(), len, nullptr, nullptr));
    return out;
}

inline std::filesystem::path GetModuleFsPath(HMODULE hModule) {
    std::wstring path(MAX_PATH, L'\0');
    DWORD actual = 0;
    while (true) {
        actual = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));
        THROW_LAST_ERROR_IF(actual == 0);
        if (static_cast<size_t>(actual) >= path.size())
            path.resize(path.size() * 2);
        else
            break;
    }
    path.resize(actual);
    return std::filesystem::path(path);
}

inline std::optional<std::vector<uint8_t>> LoadResourceData(HMODULE hInst, int id, const wchar_t* type) {
    auto hRes = FindResourceW(hInst, MAKEINTRESOURCEW(id), type);
    if (!hRes) return std::nullopt;
    auto size = SizeofResource(hInst, hRes);
    if (size == 0) return std::nullopt;
    auto hData = LoadResource(hInst, hRes);
    if (!hData) return std::nullopt;
    auto* ptr = static_cast<const uint8_t*>(LockResource(hData));
    if (!ptr) return std::nullopt;
    return std::vector<uint8_t>(ptr, ptr + size);
}

inline HWND GetWindowHandle(winrt::Microsoft::UI::Xaml::Window const& window) {
    HWND hwnd = nullptr;
    if (window) {
        winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
    }
    return hwnd;
}

inline std::filesystem::path GetLogPath() {
    std::wstring base(MAX_PATH, L'\0');
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    if (len == 0 || len >= base.size()) {
        base.assign(MAX_PATH, L'\0');
        len = GetTempPathW(static_cast<DWORD>(base.size()), base.data());
    }
    if (len == 0 || len >= base.size()) return {};

    base.resize(len);
    auto dir = std::filesystem::path(base) / L"AudioPlaybackConnector2";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir / L"AudioPlaybackConnector2.log";
}

inline void RotateLogIfNeeded(std::filesystem::path const& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return;

    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    constexpr ULONGLONG c_maxLogBytes = 2ull * 1024ull * 1024ull;
    if (size.QuadPart < c_maxLogBytes) return;

    auto backup = path;
    backup += L".1";
    DeleteFileW(backup.c_str());
    MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline void WriteLogLine(std::wstring_view message) noexcept {
    static std::mutex s_logMutex;
    std::scoped_lock lock(s_logMutex);

    try {
        auto path = GetLogPath();
        if (path.empty()) return;
        RotateLogIfNeeded(path);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        auto line = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [T:{}] {}\r\n",
                                st.wYear,
                                st.wMonth,
                                st.wDay,
                                st.wHour,
                                st.wMinute,
                                st.wSecond,
                                st.wMilliseconds,
                                GetCurrentThreadId(),
                                message);
        auto utf8 = Utf16ToUtf8(line);
        wil::unique_hfile file(CreateFileW(path.c_str(),
                                           FILE_APPEND_DATA,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr,
                                           OPEN_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr));
        if (!file) return;

        DWORD written = 0;
        WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    } catch (...) {
    }
}

} // namespace util

/* Window Constants */
/*------------------------------------------------------------------------------------------------------------------*/

inline constexpr int32_t c_settingsWindowWidth = 540;
inline constexpr int32_t c_settingsWindowHeight = 580;

/* Debug Logging */
/*------------------------------------------------------------------------------------------------------------------*/

inline void DebugTrace(std::wstring_view message) {
    util::WriteLogLine(message);
#ifdef _DEBUG
    OutputDebugStringW(std::wstring(message).c_str());
    OutputDebugStringW(L"\n");
#endif
}
template <typename... Args>
inline void DebugTrace(std::wstring_view fmt, Args&&... args) {
    auto msg = std::vformat(fmt, std::make_wformat_args(args...));
    util::WriteLogLine(msg);
#ifdef _DEBUG
    OutputDebugStringW(msg.c_str());
    OutputDebugStringW(L"\n");
#endif
}
