#pragma once

#include <winrt/Windows.ApplicationModel.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Store Channel Detection ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace apc::app {

// The Store channel is a packaging property: only the Store build carries the
// rewritten package identity name (see StorePackageIdentityName in the wapproj).
// Unpackaged processes are never the Store channel.
[[nodiscard]] inline bool IsStoreChannel() noexcept {
    try {
        return winrt::Windows::ApplicationModel::Package::Current().Id().Name() ==
               L"12144NoahMeyer.AudioPlaybackConnector2";
    } catch (...) {
        return false;
    }
}

} // namespace apc::app
