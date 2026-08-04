#include <app/SingleInstanceGuard.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool SingleInstanceGuard::TryAcquire(std::wstring_view mutexName) noexcept {
    if (mutexName.empty()) return false;
    if (m_mutex) return std::wstring_view(m_mutexName) == mutexName;

    try {
        std::wstring name(mutexName);
        auto rawMutex = CreateMutexW(nullptr, FALSE, name.c_str());
        auto const createError = GetLastError();
        wil::unique_handle mutex(rawMutex);
        if (!mutex || createError == ERROR_ALREADY_EXISTS) return false;

        m_mutex = std::move(mutex);
        m_mutexName = std::move(name);
        return true;
    } catch (...) {
        return false;
    }
}

void SingleInstanceGuard::Release() noexcept {
    m_mutex.reset();
    m_mutexName.clear();
}
