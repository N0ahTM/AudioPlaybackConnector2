#pragma once

#include <app/AppController.hpp>
#include <app/AppModels.hpp>

#include <functional>
#include <memory>
#include <utility>

namespace apc::ui {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Tray Primary Activation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

using TrayPrimaryActivationCallback = std::move_only_function<void()>;

[[nodiscard]] inline TrayPrimaryActivationCallback
MakeTrayPrimaryActivationCallback(std::weak_ptr<apc::app::AppController> controller) {
    return [controller = std::move(controller)] {
        auto owner = controller.lock();
        if (!owner) return;

        // The tray callback runs on the UI thread and cannot wait for its own Opened event.
        apc::app::AppCommandContext context;
        context.Completion = apc::app::AppCommandContext::CompletionMode::Detached;
        (void)owner->ShowDevicePicker(apc::app::DevicePickerOpenMode::ToggleIfOpen, context);
    };
}

} // namespace apc::ui
