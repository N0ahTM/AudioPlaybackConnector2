#pragma once

#include <roapi.h>

namespace util {

class RuntimeApartment final {
public:
    RuntimeApartment() noexcept : m_result(RoInitialize(RO_INIT_MULTITHREADED)) {}

    ~RuntimeApartment() {
        if (SUCCEEDED(m_result)) RoUninitialize();
    }

    RuntimeApartment(RuntimeApartment const&) = delete;
    RuntimeApartment& operator=(RuntimeApartment const&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE; }

    [[nodiscard]] HRESULT Result() const noexcept { return m_result; }

private:
    HRESULT m_result = E_FAIL;
};

} // namespace util
