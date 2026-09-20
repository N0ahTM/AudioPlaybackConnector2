#pragma once

#include <cstdint>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Startup Task Snapshot /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct StartupTaskSnapshot {
    std::uint64_t Publication = 0;
    std::uint64_t Revision = 0;
    bool Enabled = false;
    bool Known = false;
    bool Busy = false;
    bool Failed = false;

    friend bool operator==(StartupTaskSnapshot const&, StartupTaskSnapshot const&) = default;
};
