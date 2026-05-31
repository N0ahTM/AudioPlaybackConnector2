#pragma once

#include <chrono>
#include <tuple>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Template ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

template <typename... Args> class Event {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using Handler = std::function<void(Args...)>;
    using HandlerId = std::size_t;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    HandlerId operator+=(Handler handler) {
        auto guard = m_lock.lock_exclusive();
        HandlerId id = m_nextId++;
        m_handlers.push_back({id, std::make_shared<Handler>(std::move(handler))});
        return id;
    }

    void operator-=(HandlerId id) {
        auto guard = m_lock.lock_exclusive();
        std::erase_if(m_handlers, [id](const auto& pair) { return pair.first == id; });
    }

    template <typename... CallArgs> void operator()(CallArgs&&... args) const {
        static_assert(sizeof...(CallArgs) == sizeof...(Args), "Event argument count mismatch");
        auto snapshot = std::tuple<std::decay_t<Args>...>(std::forward<CallArgs>(args)...);
        std::vector<std::shared_ptr<Handler>> copy;
        {
            auto guard = m_lock.lock_shared();
            copy.reserve(m_handlers.size());
            for (auto& h : m_handlers) {
                copy.push_back(h.second);
            }
        }
        for (auto& h : copy) {
            try {
                std::apply([&](auto const&... values) { (*h)(values...); }, snapshot);
            } catch (...) {
            }
        }
    }

    void clear() {
        auto guard = m_lock.lock_exclusive();
        m_handlers.clear();
    }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::vector<std::pair<HandlerId, std::shared_ptr<Handler>>> m_handlers;
    HandlerId m_nextId = 1;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// String Helpers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

inline std::wstring Utf8ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for MultiByteToWideChar");
    }
    const int len =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(len == 0);
    std::wstring out(len, L'\0');
    THROW_LAST_ERROR_IF(
        0 == MultiByteToWideChar(
                 CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), len));
    return out;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for WideCharToMultiByte");
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(len == 0);
    std::string out(len, '\0');
    THROW_LAST_ERROR_IF(0 == WideCharToMultiByte(CP_UTF8,
                                                 WC_ERR_INVALID_CHARS,
                                                 utf16.data(),
                                                 static_cast<int>(utf16.size()),
                                                 out.data(),
                                                 len,
                                                 nullptr,
                                                 nullptr));
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

} // namespace util

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Window Constants //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline constexpr int32_t c_settingsWindowWidth = 540;
inline constexpr int32_t c_settingsWindowHeight = 580;
