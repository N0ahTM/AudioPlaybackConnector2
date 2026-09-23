#pragma once

#include <app/AppModels.hpp>

namespace apc::app {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Presentation Contract /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class AppActionStatus { Succeeded, Failed, Cancelled, TimedOut, Indeterminate };

struct AppUiActionResult {
    AppActionStatus Status = AppActionStatus::Failed;
    std::optional<std::uint64_t> DevicePickerOpenedGeneration;
};

// The UI boundary owns dispatcher admission and window acknowledgement. It contains no device or settings actions.
class AppPresentation {
public:
    virtual ~AppPresentation() = default;
    virtual AppUiActionResult PresentDevicePicker(DevicePickerOpenMode mode, AppCommandContext const& context) = 0;
    virtual AppUiActionResult PresentSettings(AppCommandContext const& context) = 0;
    virtual AppSnapshot::ResourceStatusSnapshot ResourceStatus() const = 0;
    virtual std::uint64_t PickerOpenedGeneration() const = 0;
};

} // namespace apc::app
