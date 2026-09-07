#pragma once

#include <app/AppModels.hpp>

#include <functional>
#include <string_view>
#include <utility>

namespace apc::app {

// The settings window emits the same typed default/alias commands as the
// control adapter.  The callback is supplied by composition, so this helper
// does not own an application controller or any settings state.
class SettingsWindowCommandExecutor final {
public:
    using ExecuteCallback = std::function<AppResult(AppCommand)>;

    explicit SettingsWindowCommandExecutor(ExecuteCallback execute) : m_execute(std::move(execute)) {}

    [[nodiscard]] AppResult ClearDefault() const noexcept {
        return Dispatch(AppCommand{AppCommandKind::ClearDefault, {}, {}});
    }

    [[nodiscard]] AppResult SetDefault(std::wstring_view deviceId) const noexcept {
        auto target = DeviceSelector::ById(deviceId);
        if (!target) return Invalid(AppCommandKind::SetDefault);
        return Dispatch(AppCommand{AppCommandKind::SetDefault, std::move(*target), {}});
    }

    [[nodiscard]] AppResult SetAlias(std::wstring_view deviceId, std::wstring_view alias) const noexcept {
        if (alias.empty()) return ClearAlias(deviceId);
        auto target = DeviceSelector::ById(deviceId);
        if (!target) return Invalid(AppCommandKind::SetAlias);
        return Dispatch(AppCommand{AppCommandKind::SetAlias, std::move(*target), std::wstring(alias)});
    }

    [[nodiscard]] AppResult ClearAlias(std::wstring_view deviceId) const noexcept {
        auto target = DeviceSelector::ById(deviceId);
        if (!target) return Invalid(AppCommandKind::ClearAlias);
        return Dispatch(AppCommand{AppCommandKind::ClearAlias, std::move(*target), {}});
    }

private:
    [[nodiscard]] AppResult Dispatch(AppCommand command) const noexcept {
        if (!m_execute) return Unavailable(command.Kind);
        try {
            return m_execute(std::move(command));
        } catch (...) {
            AppResult result;
            result.Code = AppResultCode::InternalError;
            result.Command = command.Kind;
            result.Reason = AppOutcomeReason::InternalError;
            return result;
        }
    }

    [[nodiscard]] static AppResult Invalid(AppCommandKind command) noexcept {
        AppResult result;
        result.Code = AppResultCode::InvalidInput;
        result.Command = command;
        result.Reason = AppOutcomeReason::TargetRequired;
        return result;
    }

    [[nodiscard]] static AppResult Unavailable(AppCommandKind command) noexcept {
        AppResult result;
        result.Code = AppResultCode::Unavailable;
        result.Command = command;
        result.Reason = AppOutcomeReason::NotReady;
        return result;
    }

    ExecuteCallback m_execute;
};

} // namespace apc::app
