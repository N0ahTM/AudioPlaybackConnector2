#pragma once

#include <app/AppModels.hpp>

#include <functional>

namespace apc::ui {

// Tray primary activation is a UI intent, not an AppController dependency.
// The UI boundary only receives a no-argument callback.  The executor seam
// owns both the P09 toggle command and detached completion required when the
// tray callback is already running on the XAML dispatcher thread.
using TrayPrimaryActivationCallback = std::move_only_function<void()>;
using TrayPrimaryActivationExecutor = std::move_only_function<void(apc::app::AppCommand, apc::app::AppCommandContext)>;

[[nodiscard]] inline apc::app::AppCommand MakeTrayPrimaryActivationCommand() {
    return {apc::app::AppCommandKind::ShowDevicePicker, {}, {}, apc::app::DevicePickerOpenMode::ToggleIfOpen};
}

[[nodiscard]] inline TrayPrimaryActivationCallback
MakeTrayPrimaryActivationCallback(TrayPrimaryActivationExecutor executor) {
    return [executor = std::move(executor)]() mutable {
        if (!executor) return;

        apc::app::AppCommandContext context;
        context.Completion = apc::app::AppCommandContext::CompletionMode::Detached;
        executor(MakeTrayPrimaryActivationCommand(), context);
    };
}

} // namespace apc::ui
